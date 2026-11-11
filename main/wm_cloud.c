#include "wm_runtime_policy.h"
// main/wm_cloud.c —— 云端通信实现。约定见 wm_cloud.h,接口契约见 docs/wemeet/device-api.md。
#include "wm_cloud.h"
#include "wm_model.h"
#include "wm_net.h"
#include "wm_proto.h"     // wm_utf8_valid
#include "wm_store.h"

#include "bsp_battery.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "wm_cloud";

#define MODEL_NAME      "wemeet-passport-c3"
#define HTTP_TIMEOUT_MS 8000
#define RESPONSE_MAX    8193     // 单次响应上限;服务器按 device-api.md 的条数限制裁剪内容
#define TOKEN_MAX       128      // 设备令牌最大长度(含结尾 NUL)

// 工作任务的请求项。
typedef struct {
    wm_cloud_op_t op;
    char arg[WM_TOKEN_MAX + 1];
} request_t;

static wm_cloud_cb_t s_cb;
static QueueHandle_t s_queue;          // 待执行请求
static SemaphoreHandle_t s_model_mutex;
static wm_model_t s_model;             // 全局数据模型(受 s_model_mutex 保护)
static char s_response[RESPONSE_MAX];  // 响应缓冲;只在工作任务里使用
static int s_response_size;           // Complete response length, including binary portraits
static int s_tz_offset_min = 480;      // 活动时区相对 UTC 的分钟数;默认北京时间
static bool s_time_valid;              // 是否已用服务器时间校过时

wm_model_t *wm_model_lock(void) {
    if (!s_model_mutex) return NULL;
    return xSemaphoreTake(s_model_mutex, pdMS_TO_TICKS(1000)) == pdTRUE ? &s_model : NULL;
}

void wm_model_unlock(void) {
    xSemaphoreGive(s_model_mutex);
}

// 把 JSON 字符串字段安全地拷进定长缓冲:缺失/非字符串 → 空串;超长 → 在 UTF-8 字符
// 边界截断;非法 UTF-8 → "?"(宁可显示问号也不把乱码交给 LVGL)。
static void copy_text(char *dst, size_t cap, const cJSON *item) {
    dst[0] = '\0';
    if (!cJSON_IsString(item) || item->valuestring == NULL) return;
    const char *src = item->valuestring;
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    if (!wm_utf8_valid((const uint8_t *)src, n)) {
        strlcpy(dst, "?", cap);
        return;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int json_int(const cJSON *item, int fallback) {
    return cJSON_IsNumber(item) ? (int)item->valuedouble : fallback;
}

// 执行一次 HTTP 请求,把响应体读进 s_response(保证 NUL 结尾)。
// 返回 HTTP 状态码;网络/TLS 层失败返回 -1。阻塞,最长约 HTTP_TIMEOUT_MS × 阶段数。
static int http_json(esp_http_client_method_t method, const char *path, const char *bearer,
                     const char *body) {
    char url[320];
    char base[WM_URL_MAX + 1];
    wm_store_api_base(base, sizeof(base));
    // TLS certificate dates need a clock before the first HTTPS request.
    if (wm_https_needs_clock(base, time(NULL))) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) return -1;
    }
    snprintf(url, sizeof(url), "%s/device/v1%s", base, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,   // https 用内置根证书包校验;http 时被忽略
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    char auth[TOKEN_MAX + 8];
    if (bearer && bearer[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    int body_len = body ? (int)strlen(body) : 0;
    if (body) esp_http_client_set_header(client, "Content-Type", "application/json");

    int status = -1;
    s_response[0] = '\0';
    s_response_size = 0;
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err == ESP_OK && body_len > 0 &&
        esp_http_client_write(client, body, body_len) != body_len) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        int total = 0;
        while (total < RESPONSE_MAX - 1) {
            int got = esp_http_client_read(client, s_response + total, RESPONSE_MAX - 1 - total);
            if (got <= 0) break;
            total += got;
        }
        s_response[total] = '\0';
        if (!esp_http_client_is_complete_data_received(client)) status = -1;
        else s_response_size = total;
    } else {
        ESP_LOGW(TAG, "%s 请求失败: %s", path, esp_err_to_name(err));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

// 把非 2xx 的状态码归类,并把服务器的 error.message 存进 model.error 供界面显示。
static wm_cloud_status_t classify_failure(int status) {
    if (status < 0) return WM_CLOUD_ERR_NETWORK;
    char message[sizeof(s_model.error)] = "";
    cJSON *root = cJSON_Parse(s_response);
    if (root) {
        const cJSON *error = cJSON_GetObjectItem(root, "error");
        copy_text(message, sizeof(message), cJSON_GetObjectItem(error, "message"));
        cJSON_Delete(root);
    }
    wm_model_t *m = wm_model_lock();
    if (m) {
        strlcpy(m->error, message, sizeof(m->error));
        wm_model_unlock();
    }
    if (status == 401 || status == 403) return WM_CLOUD_ERR_AUTH;
    if (status >= 400 && status < 500) return WM_CLOUD_ERR_REJECTED;
    return WM_CLOUD_ERR_SERVER;
}

// 认领/配对成功后的公共处理:保存设备令牌与账号名。
static bool store_binding(const cJSON *root) {
    const cJSON *token = cJSON_GetObjectItem(root, "deviceToken");
    if (!cJSON_IsString(token) || strlen(token->valuestring) == 0 ||
        strlen(token->valuestring) >= TOKEN_MAX) {
        return false;
    }
    char owner[sizeof(s_model.owner)];
    copy_text(owner, sizeof(owner), cJSON_GetObjectItem(cJSON_GetObjectItem(root, "owner"), "name"));
    if (wm_store_set(WM_KEY_TOKEN, token->valuestring) != ESP_OK) return false;
    wm_store_set(WM_KEY_OWNER, owner);
    wm_model_t *m = wm_model_lock();
    if (m) {
        strlcpy(m->owner, owner, sizeof(m->owner));
        m->rev = 0;              // 新绑定:强制下一次同步拉全量
        m->pair_code[0] = '\0';
        wm_model_unlock();
    }
    return true;
}

// 组装 {"deviceId","secret","model","fw"[,"token"]} 请求体。
static void identity_body(char *out, size_t cap, const char *token) {
    char id[20], secret[33];
    wm_store_device_id(id, sizeof(id));
    wm_store_secret(secret, sizeof(secret));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceId", id);
    cJSON_AddStringToObject(root, "secret", secret);
    cJSON_AddStringToObject(root, "model", MODEL_NAME);
    cJSON_AddStringToObject(root, "fw", esp_app_get_description()->version);
    if (token) cJSON_AddStringToObject(root, "token", token);
    if (!cJSON_PrintPreallocated(root, out, (int)cap, false)) out[0] = '\0';
    cJSON_Delete(root);
}

static wm_cloud_status_t do_claim(const char *token) {
    char body[320];
    identity_body(body, sizeof(body), token);
    int status = http_json(HTTP_METHOD_POST, "/claim", NULL, body);
    if (status != 200) return classify_failure(status);
    cJSON *root = cJSON_Parse(s_response);
    bool ok = root && store_binding(root);
    cJSON_Delete(root);
    return ok ? WM_CLOUD_OK : WM_CLOUD_ERR_SERVER;
}

static wm_cloud_status_t do_pair_start(void) {
    char body[320];
    identity_body(body, sizeof(body), NULL);
    int status = http_json(HTTP_METHOD_POST, "/pairing", NULL, body);
    if (status != 200) return classify_failure(status);
    cJSON *root = cJSON_Parse(s_response);
    if (!root) return WM_CLOUD_ERR_SERVER;
    wm_cloud_status_t result = WM_CLOUD_ERR_SERVER;
    wm_model_t *m = wm_model_lock();
    if (m) {
        copy_text(m->pair_code, sizeof(m->pair_code), cJSON_GetObjectItem(root, "code"));
        copy_text(m->pair_url, sizeof(m->pair_url), cJSON_GetObjectItem(root, "url"));
        if (m->pair_url[0] == '\0' && m->pair_code[0]) {
            snprintf(m->pair_url, sizeof(m->pair_url), "%s?c=%s", CONFIG_WM_PAIR_URL_BASE, m->pair_code);
        }
        m->pair_ttl_sec = json_int(cJSON_GetObjectItem(root, "expiresIn"), 600);
        if (m->pair_code[0]) result = WM_CLOUD_OK;
        wm_model_unlock();
    }
    cJSON_Delete(root);
    return result;
}

static wm_cloud_status_t do_pair_poll(void) {
    char body[320];
    identity_body(body, sizeof(body), NULL);
    int status = http_json(HTTP_METHOD_POST, "/pairing/poll", NULL, body);
    if (status != 200) return classify_failure(status);
    cJSON *root = cJSON_Parse(s_response);
    if (!root) return WM_CLOUD_ERR_SERVER;
    wm_cloud_status_t result = WM_CLOUD_ERR_SERVER;
    const cJSON *state = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(state)) {
        if (strcmp(state->valuestring, "bound") == 0) {
            result = store_binding(root) ? WM_CLOUD_OK : WM_CLOUD_ERR_SERVER;
        } else if (strcmp(state->valuestring, "pending") == 0) {
            result = WM_CLOUD_PENDING;
            wm_model_t *m = wm_model_lock();
            if (m) {
                m->pair_ttl_sec = json_int(cJSON_GetObjectItem(root, "expiresIn"), m->pair_ttl_sec);
                wm_model_unlock();
            }
        } else {
            result = WM_CLOUD_ERR_REJECTED;   // "expired":需要重新申请配对码
        }
    }
    cJSON_Delete(root);
    return result;
}

// 解析一次同步响应并写入模型。调用方已持有模型锁。
static void apply_sync(wm_model_t *m, const cJSON *root) {
    // 校时:设备没有 RTC 电池,每次同步都用服务器时间对一次。
    const cJSON *now = cJSON_GetObjectItem(root, "serverTime");
    if (cJSON_IsNumber(now) && now->valuedouble > 1600000000.0) {
        struct timeval tv = { .tv_sec = (time_t)now->valuedouble, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        s_time_valid = true;
    }
    s_tz_offset_min = json_int(cJSON_GetObjectItem(root, "tzOffsetMin"), s_tz_offset_min);
    m->next_poll_sec = json_int(cJSON_GetObjectItem(root, "nextPollSec"), 0);

    // 远程命令与内容版本无关,每次都看。已有未执行的命令时不覆盖。
    const cJSON *command = cJSON_GetObjectItem(root, "command");
    if (cJSON_IsObject(command) && m->command.id == 0) {
        uint32_t id = (uint32_t)json_int(cJSON_GetObjectItem(command, "id"), 0);
        if (id != 0 && id != m->ack_id) {
            m->command.id = id;
            copy_text(m->command.type, sizeof(m->command.type), cJSON_GetObjectItem(command, "type"));
            copy_text(m->command.text, sizeof(m->command.text), cJSON_GetObjectItem(command, "text"));
            m->command.value = json_int(cJSON_GetObjectItem(command, "value"), 0);
        }
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "unchanged"))) return;

    m->rev = (uint32_t)json_int(cJSON_GetObjectItem(root, "rev"), 0);
    const cJSON *owner = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "owner"), "name");
    if (cJSON_IsString(owner)) copy_text(m->owner, sizeof(m->owner), owner);

    const cJSON *card = cJSON_GetObjectItem(root, "card");
    m->card.saved = cJSON_IsTrue(cJSON_GetObjectItem(card, "saved"));
    copy_text(m->card.name, sizeof(m->card.name), cJSON_GetObjectItem(card, "name"));
    copy_text(m->card.title, sizeof(m->card.title), cJSON_GetObjectItem(card, "title"));
    copy_text(m->card.org, sizeof(m->card.org), cJSON_GetObjectItem(card, "org"));
    const cJSON *code = cJSON_GetObjectItem(card, "contactCode");
    m->card.code[0] = '\0';
    // Never turn a truncated vCard into an incorrect contact QR.
    if (cJSON_IsString(code) && strlen(code->valuestring) < sizeof(m->card.code))
        copy_text(m->card.code, sizeof(m->card.code), code);
    char revision[65];
    copy_text(revision, sizeof(revision), cJSON_GetObjectItem(card, "avatarRevision"));
    if (strcmp(revision, m->card.avatar_revision) != 0) {
        m->card.avatar_ready = false;
        memset(m->card.avatar, 0, sizeof(m->card.avatar));
        strlcpy(m->card.avatar_revision, revision, sizeof(m->card.avatar_revision));
    }

    const cJSON *badge = cJSON_GetObjectItem(root, "badge");
    memset(&m->badge, 0, sizeof(m->badge));
    if (cJSON_IsObject(badge)) {
        m->badge.present = true;
        m->badge.checked_in = cJSON_IsTrue(cJSON_GetObjectItem(badge, "checkedIn"));
        copy_text(m->badge.event, sizeof(m->badge.event), cJSON_GetObjectItem(badge, "event"));
        copy_text(m->badge.name, sizeof(m->badge.name), cJSON_GetObjectItem(badge, "name"));
        copy_text(m->badge.title, sizeof(m->badge.title), cJSON_GetObjectItem(badge, "title"));
        copy_text(m->badge.org, sizeof(m->badge.org), cJSON_GetObjectItem(badge, "org"));
        copy_text(m->badge.code, sizeof(m->badge.code), cJSON_GetObjectItem(badge, "checkinCode"));
    }

    m->agenda_count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(root, "agenda")) {
        if (m->agenda_count >= WM_AGENDA_MAX) break;
        wm_agenda_item_t *a = &m->agenda[m->agenda_count++];
        copy_text(a->time, sizeof(a->time), cJSON_GetObjectItem(item, "time"));
        copy_text(a->title, sizeof(a->title), cJSON_GetObjectItem(item, "title"));
        copy_text(a->place, sizeof(a->place), cJSON_GetObjectItem(item, "place"));
    }

    m->notice_count = 0;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(root, "notices")) {
        if (m->notice_count >= WM_NOTICE_MAX) break;
        wm_notice_t *n = &m->notices[m->notice_count++];
        n->id = (uint32_t)json_int(cJSON_GetObjectItem(item, "id"), 0);
        copy_text(n->title, sizeof(n->title), cJSON_GetObjectItem(item, "title"));
        copy_text(n->body, sizeof(n->body), cJSON_GetObjectItem(item, "body"));
    }
}

// Cloud task only: reuse the JSON response buffer after releasing its parse tree.
// Network IO never holds the model or LVGL lock.
static void sync_portrait(const char *token) {
    char revision[65] = "";
    wm_model_t *m = wm_model_lock();
    if (m) {
        if (!m->card.avatar_ready) strlcpy(revision, m->card.avatar_revision, sizeof(revision));
        wm_model_unlock();
    }
    if (strlen(revision) != 64 || strspn(revision, "0123456789abcdef") != 64) return;
    char path[100];
    snprintf(path, sizeof(path), "/portrait?revision=%s", revision);
    int status = http_json(HTTP_METHOD_GET, path, token, NULL);
    if (status != 200 || s_response_size != WM_AVATAR_BYTES) return;
    m = wm_model_lock();
    if (m) {
        if (strcmp(revision, m->card.avatar_revision) == 0) {
            memcpy(m->card.avatar, s_response, WM_AVATAR_BYTES);
            m->card.avatar_ready = true;
        }
        wm_model_unlock();
    }
}

static wm_cloud_status_t do_sync(void) {
    char token[TOKEN_MAX];
    if (!wm_store_get(WM_KEY_TOKEN, token, sizeof(token))) return WM_CLOUD_ERR_AUTH;

    uint32_t rev = 0, ack = 0;
    wm_model_t *m = wm_model_lock();
    if (m) {
        rev = m->rev;
        ack = m->ack_id;
        wm_model_unlock();
    }
    char path[160];
    snprintf(path, sizeof(path), "/sync?rev=%u&bat=%d&rssi=%d&fw=%s&ack=%u",
             (unsigned)rev, bsp_battery_soc(), wm_net_rssi(),
             esp_app_get_description()->version, (unsigned)ack);
    int status = http_json(HTTP_METHOD_GET, path, token, NULL);
    if (status != 200) return classify_failure(status);

    cJSON *root = cJSON_Parse(s_response);
    if (!root) return WM_CLOUD_ERR_SERVER;
    m = wm_model_lock();
    if (m) {
        apply_sync(m, root);
        m->synced_ack_id = ack;
        wm_model_unlock();
    }
    cJSON_Delete(root);
    if (m) sync_portrait(token);
    return m ? WM_CLOUD_OK : WM_CLOUD_ERR_SERVER;
}

static wm_cloud_status_t do_unbind(void) {
    char token[TOKEN_MAX];
    if (wm_store_get(WM_KEY_TOKEN, token, sizeof(token)) && wm_net_state() == WM_NET_ONLINE) {
        // 尽力而为:通知不到服务器也照样清本机,PC 端仍可手动移除这台设备。
        (void)http_json(HTTP_METHOD_POST, "/unbind", token, "{}");
    }
    wm_cloud_forget();
    return WM_CLOUD_OK;
}

static void cloud_task(void *arg) {
    (void)arg;
    request_t req;
    for (;;) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) continue;
        wm_cloud_status_t status;
        switch (req.op) {
        case WM_CLOUD_OP_CLAIM:      status = do_claim(req.arg); break;
        case WM_CLOUD_OP_PAIR_START: status = do_pair_start(); break;
        case WM_CLOUD_OP_PAIR_POLL:  status = do_pair_poll(); break;
        case WM_CLOUD_OP_SYNC:       status = do_sync(); break;
        case WM_CLOUD_OP_UNBIND:     status = do_unbind(); break;
        default:                     status = WM_CLOUD_ERR_SERVER; break;
        }
        memset(req.arg, 0, sizeof(req.arg));   // 绑定令牌用完即清
        if (s_cb) s_cb(req.op, status);
    }
}

esp_err_t wm_cloud_init(wm_cloud_cb_t cb) {
    s_cb = cb;
    s_model_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(4, sizeof(request_t));
    if (!s_model_mutex || !s_queue) return ESP_ERR_NO_MEM;
    wm_store_get(WM_KEY_OWNER, s_model.owner, sizeof(s_model.owner));
    // TLS 握手(ECC)在调用任务的栈上进行,8 KB 是 esp_http_client + mbedTLS 的常用安全值。
    if (xTaskCreate(cloud_task, "wm_cloud", 8192, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool wm_cloud_request(wm_cloud_op_t op, const char *arg) {
    if (!s_queue) return false;
    request_t req = { .op = op };
    if (arg) strlcpy(req.arg, arg, sizeof(req.arg));
    return xQueueSend(s_queue, &req, 0) == pdTRUE;
}

bool wm_cloud_is_bound(void) {
    char token[TOKEN_MAX];
    return wm_store_get(WM_KEY_TOKEN, token, sizeof(token));
}

void wm_cloud_forget(void) {
    wm_store_set(WM_KEY_TOKEN, NULL);
    wm_store_set(WM_KEY_OWNER, NULL);
    wm_model_t *m = wm_model_lock();
    if (m) {
        memset(m, 0, sizeof(*m));
        wm_model_unlock();
    }
}

int wm_cloud_local_minutes(void) {
    if (!s_time_valid) return -1;
    time_t now = time(NULL) + (time_t)s_tz_offset_min * 60;
    return (int)((now % 86400) / 60);
}
