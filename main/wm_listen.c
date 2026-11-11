// main/wm_listen.c —— "接收命令"模式实现。约定见 wm_listen.h。
#include "wm_listen.h"
#include "wm_sonic.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "wm_listen";

// 每次从 codec 读 256 个采样(16 ms)。再大界面电平表会发钝,再小任务切换开销变高。
#define CHUNK_SAMPLES 256

static wm_sonic_t s_rx;                 // 解调器实例(约 4.8 KB,静态分配)
static wm_listen_cb_t s_cb;
static TaskHandle_t s_task;             // 非 NULL = 接收任务存活
static SemaphoreHandle_t s_done;        // 任务退出前给出,stop() 靠它等待
static volatile bool s_run;             // stop() 置 false 请求任务退出
// 去重:H5 会把同一帧重播几遍,(cmd, seq, 长度) 相同的只上报一次。
static uint8_t s_last_type, s_last_seq, s_last_len;
static bool s_have_last;

static void listen_task(void *arg) {
    (void)arg;
    static int16_t pcm[CHUNK_SAMPLES];
    static wm_cmd_t cmd;                // 静态:结构体约 330 字节,不占任务栈
    while (s_run) {
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            ESP_LOGE(TAG, "麦克风读取失败");
            if (s_cb) s_cb(WM_LISTEN_EV_AUDIO_ERROR, NULL);
            break;
        }
        wm_sonic_event_t ev = wm_sonic_feed(&s_rx, pcm, CHUNK_SAMPLES);
        if (ev == WM_SONIC_EV_BAD_BODY) {
            if (s_cb) s_cb(WM_LISTEN_EV_BAD_FRAME, NULL);
        } else if (ev == WM_SONIC_EV_FRAME) {
            ESP_LOGI(TAG, "收到一帧 %d 字节,纠正 %d 字节", s_rx.payload_len, s_rx.corrected);
            wm_proto_err_t err = wm_proto_parse(s_rx.payload, s_rx.payload_len, &cmd);
            bool repeat = s_have_last && cmd.type == s_last_type && cmd.seq == s_last_seq &&
                          s_rx.payload_len == s_last_len;
            s_last_type = cmd.type;
            s_last_seq = cmd.seq;
            s_last_len = s_rx.payload_len;
            s_have_last = true;
            if (!repeat && s_cb) {
                s_cb(err == WM_PROTO_OK ? WM_LISTEN_EV_COMMAND : WM_LISTEN_EV_UNSUPPORTED, &cmd);
            }
            // 命令里可能有 Wi-Fi 密码:上报后立刻把两处缓冲清掉。
            memset(&cmd, 0, sizeof(cmd));
            memset(s_rx.payload, 0, sizeof(s_rx.payload));
        }
    }
    xSemaphoreGive(s_done);
    // 等 stop() 来删除本任务;自己不再碰任何共享状态。
    for (;;) vTaskSuspend(NULL);
}

esp_err_t wm_listen_start(wm_listen_cb_t cb) {
    if (s_task) return ESP_OK;
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
        if (!s_done) return ESP_ERR_NO_MEM;
    }
    // codec 平时处于休眠:先 wake(幂等),再确认格式。休眠状态下直接 set_format 会被 BSP 拒绝。
    esp_err_t err = bsp_audio_wake();
    if (err == ESP_OK) err = bsp_audio_set_format(WM_SONIC_SAMPLE_RATE, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开麦克风失败: %s", esp_err_to_name(err));
        return err;
    }
    s_cb = cb;
    s_have_last = false;
    wm_sonic_init(&s_rx);
    s_run = true;
    // 栈:解调窗 960 B + RS 译码约 0.7 KB + 回调链,4 KB 留有余量。优先级高于界面(保证不丢采样)。
    if (xTaskCreate(listen_task, "wm_listen", 4096, NULL, 6, &s_task) != pdPASS) {
        s_task = NULL;
        s_run = false;
        bsp_audio_sleep();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wm_listen_stop(void) {
    TaskHandle_t task = s_task;
    if (!task) return;
    s_run = false;
    // 任务最多阻塞在一次 16 ms 的读取里;1 秒还没退出说明 I2S 卡死,只能强行删除。
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "接收任务未按时退出,强制删除");
    }
    vTaskDelete(task);
    s_task = NULL;
    memset(&s_rx, 0, sizeof(s_rx));
    // PCM 读写已停止,可以安全地让 ES8311 进入低功耗。
    bsp_audio_sleep();
}

void wm_listen_get(wm_listen_status_t *out) {
    out->receiving = s_task && (s_rx.state == WM_SONIC_HEADER || s_rx.state == WM_SONIC_BODY);
    out->progress = s_rx.progress;
    out->level = s_rx.level;
}
