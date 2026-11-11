// main/wm_ui.h —— WeMeet Passport 界面层(LVGL)。
//
// 视觉语言与 WeMeet Next Web 端一致:浅色锌灰底、白色卡片、墨黑文字,青色只留给 AI /
// 声波(聆听光球)。屏幕 240×320,四角被 BSP 以 30 px 圆角裁成黑色,所以状态栏和底部
// 页码点都向内收。
//
// 线程模型:本文件的函数都会创建/删除 LVGL 对象,调用方必须已持有 bsp_lvgl_lock()
// (或本身运行在 LVGL 任务里)。每个 wm_ui_show_*() 都会清空上一页并重建,
// 传入的字符串在函数返回前被 LVGL 拷贝,调用方无需保持其有效。
#pragma once

#include "wm_model.h"
#include <stdbool.h>

// 状态栏数据。每次 show_* 时一并传入。
typedef struct {
    int battery;       // 电量 0..100;-1 = 读不到(隐藏电池图标)
    int wifi_bars;     // 信号格数 0..4;-1 = 未联网
    int minutes;       // 当天第几分钟(0..1439);-1 = 未校时(中间显示 --:--)
} wm_ui_status_t;

// 主界面的四个分页,顺序即上下键翻页顺序。
typedef enum {
    WM_PAGE_CARD = 0,
    WM_PAGE_AGENDA,
    WM_PAGE_NOTICES,
    WM_PAGE_DEVICE,
    WM_PAGE_COUNT,
} wm_page_t;

// "连接中"页面的三个步骤各自的状态。
typedef enum {
    WM_STEP_PENDING = 0,
    WM_STEP_ACTIVE,
    WM_STEP_DONE,
    WM_STEP_FAILED,
} wm_step_state_t;

typedef enum {
    WM_MSG_OK = 0,     // 绿色对勾
    WM_MSG_ERROR,      // 红色叹号
    WM_MSG_INFO,       // 青色信息
} wm_msg_kind_t;

// "设备"分页显示的信息。
typedef struct {
    const char *device_id;
    const char *owner;     // 空串 = 未绑定
    const char *ssid;      // 空串 = 未配网
    const char *firmware;
    bool online;
} wm_ui_device_t;

// 创建根屏幕与公共样式。LVGL 初始化之后、任何 show_* 之前调用一次。
void wm_ui_init(void);

// 开箱引导页:未配网时的首页。
void wm_ui_show_welcome(const wm_ui_status_t *st);

// 声波接收页:聆听光球随输入电平呼吸,收帧时显示进度。内部定时器轮询 wm_listen_get()。
// note 非 NULL 时显示在光球下方(例如上一次接收失败的原因)。
void wm_ui_show_listen(const wm_ui_status_t *st, const char *note);

// 连接页:Wi-Fi → 服务器 → 绑定账号 三步进度。detail 显示在标题下(如 SSID)。
// with_bind = false 时不显示第三步(仅重新配网)。
void wm_ui_show_steps(const wm_ui_status_t *st, const char *detail,
                      wm_step_state_t wifi, wm_step_state_t server, wm_step_state_t bind,
                      bool with_bind);

// 配对码页(兜底绑定):二维码 + 6 位码。code 为空串时显示"正在获取"。
void wm_ui_show_pairing(const wm_ui_status_t *st, const char *code, const char *url, int ttl_sec);

// 主界面四个分页。model 由调用方持锁传入。
void wm_ui_show_card(const wm_ui_status_t *st, const wm_model_t *model);
void wm_ui_show_agenda(const wm_ui_status_t *st, const wm_model_t *model);
void wm_ui_show_notices(const wm_ui_status_t *st, const wm_model_t *model, bool has_unread);
void wm_ui_show_device(const wm_ui_status_t *st, const wm_ui_device_t *info);

void wm_ui_show_notice_list(const wm_ui_status_t *st, const wm_model_t *model, int selected);
void wm_ui_show_notice_detail(const wm_ui_status_t *st, const wm_notice_t *notice, int scroll);
int wm_ui_scroll_notice(int delta);

// 名片码放大页:整屏白底大二维码,方便闸机/手机扫码。
void wm_ui_show_qr(const char *code, const char *caption);

// 纵向菜单。items 最多 6 项;selected 为高亮项下标。
void wm_ui_show_menu(const wm_ui_status_t *st, const char *title, const char *const *items,
                     int count, int selected);

// 整页提示。hint 显示在底部(如"按任意键继续"),可为 NULL。
void wm_ui_show_message(const wm_ui_status_t *st, wm_msg_kind_t kind, const char *title,
                        const char *body, const char *hint);
