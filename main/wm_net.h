// main/wm_net.h —— Wi-Fi STA 联网。只支持 2.4 GHz(ESP32-C3 硬件限制)。
#pragma once

#include "esp_err.h"

typedef enum {
    WM_NET_IDLE = 0,     // 未配置或已主动断开
    WM_NET_CONNECTING,   // 正在连接 / 掉线后自动重连中
    WM_NET_ONLINE,       // 已拿到 IP
    WM_NET_FAILED,       // 首次连接在重试上限内失败(见 fail 原因),不再自动重试
} wm_net_state_t;

typedef enum {
    WM_NET_FAIL_NONE = 0,
    WM_NET_FAIL_NOT_FOUND,   // 扫不到这个 SSID(不在范围内,或是纯 5 GHz 网络)
    WM_NET_FAIL_AUTH,        // 密码错误
    WM_NET_FAIL_OTHER,       // 其他(路由器拒绝、DHCP 超时等)
} wm_net_fail_t;

// 状态变化回调。运行在系统事件任务(请求入队失败时为调用任务):只能做入队之类的轻量操作,
// 不得阻塞、不得访问 LVGL。
typedef void (*wm_net_cb_t)(wm_net_state_t state, wm_net_fail_t fail);

// 初始化网络栈与 Wi-Fi 驱动(不发起连接)。只调用一次;要求 NVS 已初始化。
esp_err_t wm_net_init(wm_net_cb_t cb);

// 用给定凭据(重新)连接。password 可为空串(开放网络)。复制凭据入队后返回,结果走回调。
// 首次连接最多重试 5 次后报 FAILED;曾经连上过之后掉线则无限重连(10 秒一次)。
esp_err_t wm_net_connect(const char *ssid, const char *password);

// 入队主动断开并停止重连,处理后状态回到 IDLE。
void wm_net_disconnect(void);

wm_net_state_t wm_net_state(void);

// 当前信号强度(dBm,负数);未连接返回 0。
int wm_net_rssi(void);
