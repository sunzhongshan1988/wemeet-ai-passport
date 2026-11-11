#include "wm_runtime_policy.h"
// main/wm_net.c —— Wi-Fi STA 联网实现。约定见 wm_net.h。
#include "wm_net.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "wm_net";
ESP_EVENT_DEFINE_BASE(WM_NET_CONTROL);
enum { NET_CONFIGURE, NET_DISCONNECT, NET_RETRY };

#define FIRST_CONNECT_RETRIES 5                 // 首次连接的快速重试次数
#define RECONNECT_PERIOD_US   (10 * 1000000LL)  // 掉线后的重连间隔

static wm_net_cb_t s_cb;
static volatile wm_net_state_t s_state = WM_NET_IDLE;
static bool s_waiting_start;
static unsigned s_pending_stops;
static bool s_started;                 // esp_wifi_start() 是否已调用
static bool s_want;                    // 应用是否希望保持连接(disconnect 后为 false)
static bool s_was_online;              // 本组凭据是否曾经连上过(决定重试策略)
static int s_retries;                  // 首次连接已重试次数
static esp_timer_handle_t s_retry_timer;

static void set_state(wm_net_state_t state, wm_net_fail_t fail) {
    s_state = state;
    if (s_cb) s_cb(state, fail);
}

// 把驱动的断开原因归成界面能解释的三类。
static wm_net_fail_t classify(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return WM_NET_FAIL_NOT_FOUND;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return WM_NET_FAIL_AUTH;
    default:
        return WM_NET_FAIL_OTHER;
    }
}

static void retry_timer_cb(void *arg) {
    (void)arg;
    // Driver changes and Wi-Fi callbacks share the same event task.
    if (esp_event_post(WM_NET_CONTROL, NET_RETRY, NULL, 0, 0) != ESP_OK)
        esp_timer_start_once(s_retry_timer, RECONNECT_PERIOD_US);
}

static esp_err_t stop_driver(void) {
    if (!s_started) return ESP_OK;
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK) {
        s_started = false;
        ++s_pending_stops;
    }
    return err;
}

static void configure(wifi_config_t *cfg) {
    s_want = false;
    s_waiting_start = true;
    s_was_online = false;
    s_retries = 0;
    esp_timer_stop(s_retry_timer);
    set_state(WM_NET_CONNECTING, WM_NET_FAIL_NONE);
    // Stop before changing credentials. Disconnect alone queues a callback
    // which used to reconnect before set_config and cause ESP_ERR_WIFI_STATE.
    esp_err_t err = stop_driver();
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, cfg);
    memset(cfg, 0, sizeof(*cfg));
    if (err == ESP_OK) {
        s_want = true;
        err = esp_wifi_start();
        if (err == ESP_OK) s_started = true;
    }
    if (err != ESP_OK) {
        s_want = false;
        s_waiting_start = false;
        ESP_LOGE(TAG, "发起连接失败: %s", esp_err_to_name(err));
        set_state(WM_NET_FAILED, WM_NET_FAIL_OTHER);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WM_NET_CONTROL) {
        if (id == NET_CONFIGURE) configure(data);
        else if (id == NET_DISCONNECT) {
            s_want = false;
            s_waiting_start = false;
            esp_timer_stop(s_retry_timer);
            stop_driver();
            set_state(WM_NET_IDLE, WM_NET_FAIL_NONE);
        } else if (id == NET_RETRY && s_want && s_was_online &&
                   !s_waiting_start && s_state == WM_NET_CONNECTING) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        if (s_pending_stops) --s_pending_stops;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Ignore starts from earlier requests whose stop is still queued.
        if (s_want && s_waiting_start && !s_pending_stops) {
            s_waiting_start = false;
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                s_want = false;
                set_state(WM_NET_FAILED, WM_NET_FAIL_OTHER);
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_want || s_waiting_start) return;
        const wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "Wi-Fi 断开,reason=%d", ev->reason);
        if (!s_was_online) {
            // 首次连接:快速重试几次,仍失败就把原因报给界面,让用户改密码/换网络。
            if (++s_retries <= FIRST_CONNECT_RETRIES) {
                esp_wifi_connect();
            } else {
                s_want = false;
                set_state(WM_NET_FAILED, classify(ev->reason));
            }
            return;
        }
        // 曾经连上过:多半是路由器重启或走出覆盖范围,低频无限重连即可。
        if (s_state != WM_NET_CONNECTING) set_state(WM_NET_CONNECTING, WM_NET_FAIL_NONE);
        esp_timer_stop(s_retry_timer);   // 未在运行时返回错误,可忽略
        esp_timer_start_once(s_retry_timer, RECONNECT_PERIOD_US);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (!s_want || s_waiting_start) return;
        s_was_online = true;
        s_retries = 0;
        esp_netif_sntp_start();
        set_state(WM_NET_ONLINE, WM_NET_FAIL_NONE);
    }
}

esp_err_t wm_net_init(wm_net_cb_t cb) {
    s_cb = cb;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;   // 已创建过不算错
    if (esp_netif_create_default_wifi_sta() == NULL) return ESP_ERR_NO_MEM;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    // 凭据由 wm_store 管理,不让驱动再往 NVS 存一份。
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    if (err != ESP_OK) return err;

    err = esp_event_handler_register(WM_NET_CONTROL, ESP_EVENT_ANY_ID, on_event, NULL);
    if (err != ESP_OK) return err;

    esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    time_config.start = false;
    err = esp_netif_sntp_init(&time_config);
    if (err != ESP_OK) return err;

    const esp_timer_create_args_t timer_args = { .callback = retry_timer_cb, .name = "wm_net_retry" };
    return esp_timer_create(&timer_args, &s_retry_timer);
}

esp_err_t wm_net_connect(const char *ssid, const char *password) {
    wifi_config_t cfg = {0};
    if (!wm_wifi_credentials(cfg.sta.ssid, cfg.sta.password, ssid, password))
        return ESP_ERR_INVALID_ARG;
    // 允许 WPA3 路由器(SAE 两种模式都接受);PMF 可选。
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.pmf_cfg.capable = true;
    // 扫描全部信道并选信号最强的 AP:会场里同名 AP 很多,默认"快速扫描"会连到第一个扫到的。
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    // The event loop owns the driver and connection state; posting copies cfg.
    esp_err_t err = esp_event_post(WM_NET_CONTROL, NET_CONFIGURE, &cfg, sizeof(cfg),
                                  pdMS_TO_TICKS(1000));
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK) set_state(WM_NET_FAILED, WM_NET_FAIL_OTHER);
    return err;
}

void wm_net_disconnect(void) {
    esp_err_t err = esp_event_post(WM_NET_CONTROL, NET_DISCONNECT, NULL, 0,
                                  pdMS_TO_TICKS(1000));
    if (err != ESP_OK) ESP_LOGE(TAG, "断开请求入队失败: %s", esp_err_to_name(err));
}

wm_net_state_t wm_net_state(void) {
    return s_state;
}

int wm_net_rssi(void) {
    if (s_state != WM_NET_ONLINE) return 0;
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
