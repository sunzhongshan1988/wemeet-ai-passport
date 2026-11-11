// main/wm_cloud.h —— 设备与 WeMeet Next 服务器的通信(HTTPS + JSON 轮询)。
//
// 接口契约见 docs/wemeet/device-api.md。选 HTTPS 轮询而不是 MQTT/WebSocket 的原因:
// 无 PSRAM 的 C3 上长连接 TLS 会一直占着 ~40 KB 堆;轮询只在请求期间占用,而且后端
// 只需要普通 REST 接口。实时性要求更高时再升级为长连接,数据模型不变。
//
// 所有请求在一个独立工作任务里串行执行,结果通过回调通知;界面数据写入 wm_model。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    WM_CLOUD_OP_CLAIM = 0,    // 用声波收到的绑定令牌认领设备;arg = 令牌
    WM_CLOUD_OP_PAIR_START,   // 申请配对码(兜底绑定);结果在 model.pair_*
    WM_CLOUD_OP_PAIR_POLL,    // 查询配对码是否已被用户输入
    WM_CLOUD_OP_SYNC,         // 同步参会证/日程/通知/远程命令
    WM_CLOUD_OP_UNBIND,       // 设备端主动解绑(尽力通知服务器)
} wm_cloud_op_t;

typedef enum {
    WM_CLOUD_OK = 0,
    WM_CLOUD_PENDING,        // 仅 PAIR_POLL:用户还没输入配对码
    WM_CLOUD_ERR_NETWORK,    // 连不上服务器 / 超时 / TLS 失败
    WM_CLOUD_ERR_AUTH,       // 401/403:设备令牌已失效(在 PC 端被解绑)
    WM_CLOUD_ERR_REJECTED,   // 其他 4xx:绑定令牌无效或过期等;说明文字在 model.error
    WM_CLOUD_ERR_SERVER,     // 5xx 或响应无法解析
} wm_cloud_status_t;

// 结果回调。运行在云端工作任务:只做入队等轻量操作,不得阻塞、不得访问 LVGL。
typedef void (*wm_cloud_cb_t)(wm_cloud_op_t op, wm_cloud_status_t status);

// 创建工作任务与数据模型。只调用一次;要求 NVS 已初始化。
esp_err_t wm_cloud_init(wm_cloud_cb_t cb);

// 提交一个请求(非阻塞)。arg 仅 CLAIM 使用(绑定令牌,内部拷贝),其余传 NULL。
// 队列已满时返回 false(调用方稍后重试即可)。
bool wm_cloud_request(wm_cloud_op_t op, const char *arg);

// 本机是否持有设备令牌(= 已绑定)。读 NVS,不要在高频路径调用。
bool wm_cloud_is_bound(void);

// 清除本机绑定信息(令牌、账号名、内容),不联网。
void wm_cloud_forget(void);

// 按服务器时间与活动时区给出"当天第几分钟"(0..1439);尚未同步过时间返回 -1。
int wm_cloud_local_minutes(void);
