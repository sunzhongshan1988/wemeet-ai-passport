// main/wm_model.h —— 设备端数据模型:云端同步下来、界面要显示的全部内容。
//
// 所有权与并发:全局只有一份(wm_cloud.c 持有)。云端任务在解析响应时写,应用任务在
// 渲染时读;双方都必须先 wm_model_lock()。持锁期间只做内存拷贝/建 LVGL 对象,
// 不得做网络、Flash 或音频操作。
// 字符串一律是以 NUL 结尾的合法 UTF-8;超长内容在字符边界截断。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WM_AGENDA_MAX 5
#define WM_NOTICE_MAX 4
#define WM_AVATAR_SIDE 48
#define WM_AVATAR_BYTES (WM_AVATAR_SIDE * WM_AVATAR_SIDE * 2)

typedef struct {
    bool saved;
    char name[181], title[241], org[241], code[1601];
    char avatar_revision[65];
    bool avatar_ready;
    uint8_t avatar[WM_AVATAR_BYTES]; /* little-endian RGB565, no image decoder needed */
} wm_card_t;

// 电子参会证。present = false 表示账号下当前没有可显示的活动。
typedef struct {
    bool present;
    bool checked_in;      // 已签到
    char event[64];       // 活动名称
    char name[48];        // 参会人姓名
    char title[64];       // 职位
    char org[64];         // 公司 / 组织
    char code[128];       // 签到码内容(显示为二维码);空串则不显示二维码
} wm_badge_t;

typedef struct {
    char time[12];        // 服务器已按活动时区格式化,如 "14:30"
    char title[64];
    char place[32];
} wm_agenda_item_t;

typedef struct {
    uint32_t id;          // 服务器单调递增;大于已读 id 的视为新通知
    char title[48];
    char body[160];
} wm_notice_t;

// 云端下发、等待应用任务执行的一条设备命令(一次同步最多取一条)。
typedef struct {
    uint32_t id;          // 0 = 没有待执行命令
    char type[16];        // "locate" | "message" | "reboot" | "unbind" | "brightness"
    char text[97];        // message 的文本
    int value;            // brightness 的亮度(0..100)
} wm_remote_cmd_t;

typedef struct {
    char owner[48];                        // 绑定账号的显示名
    uint32_t rev;                          // 内容版本;同步时带给服务器,未变化则服务器不重发
    wm_badge_t badge;
    wm_card_t card;
    wm_agenda_item_t agenda[WM_AGENDA_MAX];
    int agenda_count;
    wm_notice_t notices[WM_NOTICE_MAX];    // 新的在前
    int notice_count;
    wm_remote_cmd_t command;
    uint32_t synced_ack_id;                // Ack included in the last successful sync
    uint32_t ack_id;                       // 已执行、待下次同步回执的命令 id
    char pair_code[12];                    // 配对码(兜底绑定流程)
    char pair_url[128];                    // 配对页地址(显示为二维码)
    int pair_ttl_sec;                      // 配对码剩余有效期
    int next_poll_sec;                     // 服务器建议的同步间隔
    char error[80];                        // 最近一次失败时服务器给的说明(可直接显示)
} wm_model_t;

// 取全局模型并加锁 / 解锁。lock 最多等待约 1 秒,失败返回 NULL(调用方跳过本次操作)。
wm_model_t *wm_model_lock(void);
void wm_model_unlock(void);
