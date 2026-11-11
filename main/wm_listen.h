// main/wm_listen.h —— "接收命令"模式:麦克风 → 声波解调(wm_sonic)→ 命令解析(wm_proto)。
//
// 只有用户在设备上主动进入该模式时才开麦克风。这既省电,也是一道安全闸:
// 声波谁都能播,设备不在接收模式时任何声音命令都无效。
#pragma once

#include "esp_err.h"
#include "wm_proto.h"
#include <stdint.h>

typedef enum {
    WM_LISTEN_EV_COMMAND = 0,   // 收到一条合法命令;cmd 有效(仅回调期间)
    WM_LISTEN_EV_BAD_FRAME,     // 收到信号但纠错/校验失败:提示用户靠近些、调大音量再播一次
    WM_LISTEN_EV_UNSUPPORTED,   // 帧完好但命令不认识或字段非法:多半是固件版本太旧
    WM_LISTEN_EV_AUDIO_ERROR,   // 麦克风读取失败,接收任务已退出
} wm_listen_event_t;

// 事件回调。运行在接收任务:拷贝需要的数据后立即返回,不得阻塞、不得访问 LVGL。
// cmd 指向接收任务内部缓冲,回调返回后失效(且会被清零,因为里面可能有 Wi-Fi 密码)。
typedef void (*wm_listen_cb_t)(wm_listen_event_t event, const wm_cmd_t *cmd);

// 给界面看的实时状态快照。
typedef struct {
    uint8_t receiving;   // 0 = 聆听中,1 = 正在接收一帧
    uint8_t progress;    // 当前帧接收进度 0..100
    uint16_t level;      // 输入电平(RMS,0..32767)
} wm_listen_status_t;

// 打开麦克风并启动接收任务。已在运行时直接返回 ESP_OK。
// 失败(编解码器未就绪 / 内存不足)时不残留任务。会阻塞数十毫秒(配置 codec)。
esp_err_t wm_listen_start(wm_listen_cb_t cb);

// 停止接收任务并让 codec 休眠。阻塞到任务退出(最长约 1 秒)。未运行时直接返回。
// 不得在接收回调里调用。
void wm_listen_stop(void);

// 读取实时状态。任意任务可调,无锁(各字段独立,读到上一拍的值无害)。
void wm_listen_get(wm_listen_status_t *out);
