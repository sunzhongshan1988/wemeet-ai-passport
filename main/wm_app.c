#include "wm_runtime_policy.h"
#include "wm_navigation.h"
// main/wm_app.c —— WeMeet Passport 应用状态机。
//
// 结构:一个应用任务 + 一个事件队列。按键、Wi-Fi 状态、云端结果、声波命令都只是往
// 队列里投事件;所有状态转移和界面重建都在应用任务里串行发生,所以状态变量不需要加锁。
// 界面操作统一经 render()(持 LVGL 锁);云端数据经 wm_model_lock() 读取。
//
// 按键交互(三键:上 / 下 / OK):
//   主界面   上/下 = 翻页;OK 单击 = 当前页的主操作(参会证→放大签到码,设备→菜单);
//            OK 长按 = 进入"接收声波命令"
//   菜单     上/下 = 移动;OK 单击 = 确认;OK 长按 = 返回
//   聆听/连接 OK 长按 = 取消
//   提示页   任意键 = 继续
//   息屏时   任意键只点亮屏幕,不触发动作
#include "wm_app.h"
#include "wm_cloud.h"
#include "wm_listen.h"
#include "wm_model.h"
#include "wm_net.h"
#include "wm_store.h"
#include "wm_ui.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wm_app";

// ---- 时间参数(秒)----
#define DIM_AFTER_SEC        20    // 无操作多久后调暗背光
#define OFF_AFTER_SEC        60    // 无操作多久后关闭背光
#define LISTEN_TIMEOUT_SEC   120   // 聆听多久没收到命令就自动退出(麦克风 + CPU 都耗电)
#define QR_TIMEOUT_SEC       60    // 签到码放大页最长停留
#define PAIR_POLL_SEC        3     // 配对码轮询间隔
#define PAIR_RETRY_SEC       10    // 申请配对码失败后的重试间隔
#define MESSAGE_AUTO_SEC     6     // 成功类提示自动消失时间
#define BATTERY_PERIOD_SEC   30    // 电量/信号刷新间隔
#define BACKLIGHT_DIM        12    // 调暗后的亮度(%)

typedef enum {
    ST_WELCOME = 0,   // 未配网:开箱引导
    ST_LISTEN,        // 接收声波命令
    ST_CONNECT,       // 配网/绑定进行中
    ST_PAIRING,       // 已联网未绑定:显示配对码
    ST_HOME,          // 主界面(四个分页)
    ST_NOTICES,      // 通知列表或详情
    ST_QR,            // 签到码放大
    ST_MENU,          // 设备菜单或确认对话
    ST_MESSAGE,       // 整页提示
} state_t;

typedef enum {
    EV_KEY = 0,       // a = bsp_btn_t,b = bsp_btn_ev_t
    EV_SONIC_CMD,     // ptr = malloc 的 wm_cmd_t,处理后由应用任务清零并释放
    EV_SONIC_FAIL,    // a = wm_listen_event_t
    EV_NET,           // a = wm_net_state_t,b = wm_net_fail_t
    EV_CLOUD,         // a = wm_cloud_op_t,b = wm_cloud_status_t
} event_type_t;

typedef struct {
    uint8_t type;
    uint8_t a;
    uint8_t b;
    void *ptr;
} event_t;

// 菜单的三种用途。
typedef enum {
    MENU_DEVICE = 0,     // 设备菜单
    MENU_CONFIRM_UNBIND, // "解除绑定?"
    MENU_CONFIRM_RESET,  // "恢复出厂设置?"
} menu_kind_t;

// 一次配网/绑定流程的上下文。凭据只在连接成功后才写入 NVS,失败不会覆盖原有网络。
typedef struct {
    char ssid[WM_SSID_MAX + 1];
    char password[WM_PASSWORD_MAX + 1];
    char token[WM_TOKEN_MAX + 1];
    bool with_bind;                  // 本次流程是否包含"绑定到账号"
    bool new_wifi;                   // 本次流程是否带了新的 Wi-Fi 凭据
    wm_step_state_t wifi, server, bind;
} flow_t;

static QueueHandle_t s_queue;
static bool s_audio_ok, s_battery_ok;

static state_t s_state;
static wm_page_t s_page;                 // 主界面当前分页
static wm_notice_nav_t s_notice_nav;
static wm_notice_t s_notice_detail; // Stable snapshot while reading, even if sync changes the list.
static int s_notice_scroll;
static menu_kind_t s_menu_kind;
static int s_menu_sel;
static state_t s_listen_return;          // 退出聆听后回到哪里
static const char *s_listen_note;        // 聆听页的提示(上次失败原因),指向字符串常量
static flow_t s_flow;

// 提示页内容。title/body 拷贝保存,因为来源可能是即将被清零的缓冲。
static wm_msg_kind_t s_msg_kind;
static char s_msg_title[40];
static char s_msg_body[200];
static state_t s_msg_next;               // 提示页关闭后进入的状态
static int s_msg_auto_sec;               // >0:自动关闭倒计时

static char s_device_id[20];
static char s_ssid[WM_SSID_MAX + 1];     // 已保存的 Wi-Fi 名(空 = 未配网)
static bool s_bound;                     // 是否持有设备令牌(缓存,避免频繁读 NVS)
static bool s_reset_pending;             // 解绑完成后是否继续清除配网(恢复出厂)

static int s_battery = -1;               // 缓存的电量
static int s_wifi_bars = -1;             // 缓存的信号格数
static int s_minutes = -1;               // 状态栏当前显示的分钟数
static int s_idle_sec;                   // 距上次按键的秒数
static int s_backlight = -1;             // 当前背光亮度(避免重复设置 PWM)
static int s_brightness = 80;            // 亮屏亮度,可被云端 brightness 命令修改
static int s_state_sec;                  // 进入当前状态后的秒数
static int s_sync_countdown;             // 距下一次云端同步的秒数
static bool s_sync_busy;                 // 已提交同步请求、尚未返回
static int s_pair_countdown;             // 配对码页:距下一次轮询/重试的秒数
static bool s_pair_have_code;
static uint32_t s_seen_notice;           // 已提醒过的最新通知 id
static bool s_first_sync_done;           // 开机后首次同步不提醒旧通知

// ---------------------------------------------------------------------------
// 事件投递(可在任意任务调用)
// ---------------------------------------------------------------------------

static void post(event_type_t type, uint8_t a, uint8_t b, void *ptr) {
    if (!s_queue) return;
    const event_t ev = { .type = (uint8_t)type, .a = a, .b = b, .ptr = ptr };
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE && ptr) {
        // 队列满:丢弃事件,但不能泄漏(且里面可能有密码)。
        memset(ptr, 0, sizeof(wm_cmd_t));
        free(ptr);
    }
}

void wm_app_post_key(bsp_btn_t btn, bsp_btn_ev_t event) {
    post(EV_KEY, (uint8_t)btn, (uint8_t)event, NULL);
}

static void on_net(wm_net_state_t state, wm_net_fail_t fail) {
    post(EV_NET, (uint8_t)state, (uint8_t)fail, NULL);
}

static void on_cloud(wm_cloud_op_t op, wm_cloud_status_t status) {
    post(EV_CLOUD, (uint8_t)op, (uint8_t)status, NULL);
}

static void on_listen(wm_listen_event_t event, const wm_cmd_t *cmd) {
    if (event != WM_LISTEN_EV_COMMAND) {
        post(EV_SONIC_FAIL, (uint8_t)event, 0, NULL);
        return;
    }
    wm_cmd_t *copy = malloc(sizeof(*copy));
    if (!copy) return;
    *copy = *cmd;
    post(EV_SONIC_CMD, 0, 0, copy);
}

// ---------------------------------------------------------------------------
// 外设小工具
// ---------------------------------------------------------------------------

static void set_backlight(int percent) {
    if (percent == s_backlight) return;
    s_backlight = percent;
    bsp_display_backlight((uint8_t)percent);
}

// 提示音:count 声 1.2 kHz 短音。阻塞约 count × 0.2 秒(在应用任务里调用,按键会稍后处理)。
// 聆听期间不响——麦克风正在收声,自己的提示音会干扰解调。
static void beep(int count) {
    if (!s_audio_ok || s_state == ST_LISTEN) return;
    static int16_t tone[1600];   // 100 ms @16 kHz;静态分配,不占任务栈
    for (int i = 0; i < 1600; i++) {
        // 方波周期约 13 个采样 ≈ 1.2 kHz;首尾各 5 ms 线性淡入淡出避免爆音。
        int amp = 5000;
        if (i < 80) amp = amp * i / 80;
        if (i >= 1520) amp = amp * (1600 - i) / 80;
        tone[i] = (int16_t)(((i / 7) & 1) ? amp : -amp);
    }
    if (bsp_audio_wake() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK) return;
    bsp_audio_set_volume(70);
    for (int n = 0; n < count; n++) {
        bsp_audio_write(tone, sizeof(tone));
        if (n + 1 < count) vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(120));   // 等 DMA 把尾音送完再让 codec 休眠
    bsp_audio_sleep();
}

static void refresh_sensors(void) {
    s_battery = s_battery_ok ? bsp_battery_soc() : -1;
    if (wm_net_state() != WM_NET_ONLINE) {
        s_wifi_bars = -1;
    } else {
        int rssi = wm_net_rssi();
        s_wifi_bars = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
    }
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

static const char *const DEVICE_MENU_BOUND[] = { "接收声波命令", "解除绑定", "恢复出厂设置", "返回" };
static const char *const DEVICE_MENU_UNBOUND[] = { "接收声波命令", "显示配对码", "恢复出厂设置", "返回" };
static const char *const CONFIRM_UNBIND[] = { "取消", "确认解除绑定" };
static const char *const CONFIRM_RESET[] = { "取消", "确认恢复出厂" };

// 按当前状态重建界面。只在应用任务里调用。
static void render(void) {
    const wm_ui_status_t st = { .battery = s_battery, .wifi_bars = s_wifi_bars, .minutes = s_minutes };
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGW(TAG, "LVGL 锁超时,跳过一次渲染");
        return;
    }
    switch (s_state) {
    case ST_WELCOME:
        wm_ui_show_welcome(&st);
        break;
    case ST_LISTEN:
        wm_ui_show_listen(&st, s_listen_note);
        break;
    case ST_CONNECT:
        wm_ui_show_steps(&st, s_flow.new_wifi ? s_flow.ssid : s_ssid,
                         s_flow.wifi, s_flow.server, s_flow.bind, s_flow.with_bind);
        break;
    case ST_MENU:
        if (s_menu_kind == MENU_DEVICE) {
            wm_ui_show_menu(&st, "设备菜单", s_bound ? DEVICE_MENU_BOUND : DEVICE_MENU_UNBOUND, 4, s_menu_sel);
        } else if (s_menu_kind == MENU_CONFIRM_UNBIND) {
            wm_ui_show_menu(&st, "解除绑定?", CONFIRM_UNBIND, 2, s_menu_sel);
        } else {
            wm_ui_show_menu(&st, "恢复出厂?", CONFIRM_RESET, 2, s_menu_sel);
        }
        break;
    case ST_MESSAGE:
        wm_ui_show_message(&st, s_msg_kind, s_msg_title, s_msg_body,
                           s_msg_auto_sec > 0 ? NULL : "按任意键继续");
        break;
    default: {
        // 其余页面要读云端数据:在 LVGL 锁内再取模型锁(云端任务从不持 LVGL 锁,不会死锁)。
        wm_model_t *m = wm_model_lock();
        if (!m) break;
        if (s_state == ST_PAIRING) {
            wm_ui_show_pairing(&st, m->pair_code, m->pair_url, m->pair_ttl_sec);
        } else if (s_state == ST_QR) {
            wm_ui_show_qr(m->card.code, m->card.name);
        } else if (s_state == ST_NOTICES) {
            if (s_notice_nav.view == WM_NOTICE_DETAIL) {
                wm_ui_show_notice_detail(&st, &s_notice_detail, s_notice_scroll);
            } else {
                uint32_t ids[WM_NOTICE_MAX];
                for (int i = 0; i < m->notice_count; i++) ids[i] = m->notices[i].id;
                wm_notice_reconcile(&s_notice_nav, ids, m->notice_count);
                wm_ui_show_notice_list(&st, m, s_notice_nav.selected);
            }
        } else if (s_page == WM_PAGE_CARD) {
            wm_ui_show_card(&st, m);
        } else if (s_page == WM_PAGE_AGENDA) {
            wm_ui_show_agenda(&st, m);
        } else if (s_page == WM_PAGE_NOTICES) {
            wm_ui_show_notices(&st, m, m->notice_count > 0 && m->notices[0].id > s_seen_notice);
        } else {
            const wm_ui_device_t info = {
                .device_id = s_device_id,
                .owner = m->owner,
                .ssid = s_ssid,
                .firmware = esp_app_get_description()->version,
                .online = wm_net_state() == WM_NET_ONLINE,
            };
            wm_ui_show_device(&st, &info);
        }
        wm_model_unlock();
        break;
    }
    }
    bsp_lvgl_unlock();
}

// 切换状态并重绘。离开聆听态时负责关麦克风。
static void enter(state_t next) {
    if (s_state == ST_LISTEN && next != ST_LISTEN) wm_listen_stop();
    s_state = next;
    s_state_sec = 0;
    if (next == ST_PAIRING) {
        s_pair_have_code = false;
        s_pair_countdown = 0;     // 下一个 tick 立即申请配对码
        wm_model_t *m = wm_model_lock();
        if (m) {
            m->pair_code[0] = '\0';
            wm_model_unlock();
        }
    }
    if (next == ST_HOME) s_sync_countdown = 0;   // 回到主界面时尽快同步一次
    render();
}

// 显示整页提示,关闭后进入 next。auto_close 为 true 时几秒后自动关闭。
static void show_message(wm_msg_kind_t kind, const char *title, const char *body, state_t next,
                         bool auto_close) {
    s_msg_kind = kind;
    strlcpy(s_msg_title, title, sizeof(s_msg_title));
    strlcpy(s_msg_body, body ? body : "", sizeof(s_msg_body));
    s_msg_next = next;
    s_msg_auto_sec = auto_close ? MESSAGE_AUTO_SEC : 0;
    s_idle_sec = 0;               // 有新提示时点亮屏幕
    set_backlight(s_brightness);
    enter(ST_MESSAGE);
}

// 没有进行中的流程时应该停留的状态。
static state_t rest_state(void) {
    if (s_ssid[0] == '\0') return ST_WELCOME;
    return s_bound ? ST_HOME : ST_PAIRING;
}

// ---------------------------------------------------------------------------
// 流程:聆听 / 配网 / 绑定 / 重置
// ---------------------------------------------------------------------------

static void start_listen(const char *note) {
    if (!s_audio_ok) {
        show_message(WM_MSG_ERROR, "麦克风不可用", "无法接收声波命令,请重启设备后再试。", rest_state(), false);
        return;
    }
    if (s_state != ST_LISTEN) {
        s_listen_return = (s_state == ST_MENU || s_state == ST_MESSAGE || s_state == ST_QR) ? rest_state() : s_state;
        if (wm_listen_start(on_listen) != ESP_OK) {
            show_message(WM_MSG_ERROR, "麦克风不可用", "无法打开麦克风,请重启设备后再试。", rest_state(), false);
            return;
        }
    }
    s_listen_note = note;
    enter(ST_LISTEN);
}

// 用已保存的凭据连回原来的网络(新凭据失败或用户取消后调用)。
static void reconnect_saved(void) {
    char password[WM_PASSWORD_MAX + 1];
    if (s_ssid[0] == '\0') {
        wm_net_disconnect();
        return;
    }
    wm_store_get(WM_KEY_PSK, password, sizeof(password));
    wm_net_connect(s_ssid, password);
    memset(password, 0, sizeof(password));
}

// 流程结束(成功或失败)后清掉内存里的密码与令牌。
static void flow_clear(void) {
    memset(&s_flow, 0, sizeof(s_flow));
}

static void start_claim(void) {
    s_flow.server = WM_STEP_ACTIVE;
    if (!wm_cloud_request(WM_CLOUD_OP_CLAIM, s_flow.token)) {
        show_message(WM_MSG_ERROR, "设备正忙", "请稍后在 WeMeet 重新发起绑定。", rest_state(), false);
        flow_clear();
    }
}

// 处理一条声波命令。cmd 由调用方负责清零释放。
static void handle_command(const wm_cmd_t *cmd) {
    switch (cmd->type) {
    case WM_CMD_PROVISION:
        wm_listen_stop();
        s_state = ST_CONNECT;      // 先离开聆听态,beep 才会响
        beep(1);                   // 给拿着手机的人一个"收到了"的反馈
        flow_clear();
        strlcpy(s_flow.ssid, cmd->ssid, sizeof(s_flow.ssid));
        strlcpy(s_flow.password, cmd->password, sizeof(s_flow.password));
        strlcpy(s_flow.token, cmd->token, sizeof(s_flow.token));
        s_flow.new_wifi = true;
        s_flow.with_bind = cmd->has_token;
        s_flow.wifi = WM_STEP_ACTIVE;
        wm_net_connect(s_flow.ssid, s_flow.password);
        enter(ST_CONNECT);
        break;
    case WM_CMD_BIND:
        if (wm_net_state() != WM_NET_ONLINE) {
            show_message(WM_MSG_ERROR, "尚未联网", "设备还没有连上 Wi-Fi,请先在 WeMeet 中为设备配网。",
                         rest_state(), false);
            break;
        }
        wm_listen_stop();
        s_state = ST_CONNECT;
        beep(1);
        flow_clear();
        strlcpy(s_flow.token, cmd->token, sizeof(s_flow.token));
        s_flow.with_bind = true;
        s_flow.wifi = WM_STEP_DONE;
        start_claim();
        if (s_state == ST_CONNECT) enter(ST_CONNECT);
        break;
    case WM_CMD_SERVER:
        wm_store_set(WM_KEY_API, cmd->url);
        show_message(WM_MSG_OK, "服务器已更新", cmd->url, rest_state(), true);
        break;
    case WM_CMD_RESET:
        // 声波谁都能播:破坏性操作必须在设备上再按键确认一次。
        wm_listen_stop();
        s_menu_kind = MENU_CONFIRM_RESET;
        s_menu_sel = 0;
        enter(ST_MENU);
        break;
    case WM_CMD_MESSAGE:
        show_message(WM_MSG_INFO, "来自 WeMeet", cmd->text, s_listen_return, false);
        break;
    default:
        break;
    }
}

// 清除本机全部用户数据(绑定 + 配网 + 服务器覆盖值),回到开箱状态。
static void factory_reset_local(void) {
    wm_cloud_forget();
    wm_store_set(WM_KEY_SSID, NULL);
    wm_store_set(WM_KEY_PSK, NULL);
    wm_store_set(WM_KEY_API, NULL);
    s_ssid[0] = '\0';
    s_bound = false;
    s_reset_pending = false;
    wm_net_disconnect();
    refresh_sensors();
    show_message(WM_MSG_OK, "已恢复出厂", "配网与绑定信息已清除。", ST_WELCOME, true);
}

static void menu_select(void) {
    if (s_menu_kind == MENU_DEVICE) {
        switch (s_menu_sel) {
        case 0:
            start_listen(NULL);
            break;
        case 1:
            if (s_bound) {
                s_menu_kind = MENU_CONFIRM_UNBIND;
                s_menu_sel = 0;
                render();
            } else {
                enter(rest_state());   // 未绑定:配对码页或引导页
            }
            break;
        case 2:
            s_menu_kind = MENU_CONFIRM_RESET;
            s_menu_sel = 0;
            render();
            break;
        default:
            enter(rest_state());
            break;
        }
        return;
    }
    if (s_menu_sel == 0) {            // "取消"
        enter(rest_state());
        return;
    }
    // 两种确认都先尝试通知服务器解绑;恢复出厂在解绑回调里继续清除配网。
    s_reset_pending = s_menu_kind == MENU_CONFIRM_RESET;
    if (!wm_cloud_request(WM_CLOUD_OP_UNBIND, NULL)) {
        // 云端任务队列满:只清本机,PC 端仍可手动移除设备。
        wm_cloud_forget();
        post(EV_CLOUD, WM_CLOUD_OP_UNBIND, WM_CLOUD_OK, NULL);
    }
}

// ---------------------------------------------------------------------------
// 事件处理
// ---------------------------------------------------------------------------

static void on_key_event(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
    bool was_off = s_backlight == 0;
    s_idle_sec = 0;
    set_backlight(s_state == ST_QR ? 100 : s_brightness);
    if (was_off) return;   // 息屏时第一下按键只负责点亮

    bool ok_click = btn == BSP_BTN_OK && ev == BSP_BTN_CLICK;
    bool ok_long = btn == BSP_BTN_OK && ev == BSP_BTN_LONG;
    int step = ev != BSP_BTN_CLICK ? 0 : btn == BSP_BTN_UP ? -1 : btn == BSP_BTN_DOWN ? 1 : 0;

    switch (s_state) {
    case ST_WELCOME:
        if (ok_click || ok_long) start_listen(NULL);
        break;
    case ST_LISTEN:
        if (ok_long) enter(s_listen_return);
        break;
    case ST_CONNECT:
        if (ok_long) {
            flow_clear();
            reconnect_saved();
            enter(rest_state());
        }
        break;
    case ST_PAIRING:
        if (ok_long) {
            start_listen(NULL);
        } else if (ok_click) {
            s_menu_kind = MENU_DEVICE;
            s_menu_sel = 0;
            enter(ST_MENU);
        }
        break;
    case ST_HOME:
        if (ok_long) {
            start_listen(NULL);
        } else if (step) {
            s_page = (wm_page_t)(((int)s_page + step + WM_PAGE_COUNT) % WM_PAGE_COUNT);
            if (s_page == WM_PAGE_NOTICES) {
                // 翻到通知页即视为已读。
                wm_model_t *m = wm_model_lock();
                if (m) {
                    if (m->notice_count > 0) s_seen_notice = m->notices[0].id;
                    wm_model_unlock();
                }
            }
            render();
        } else if (ok_click && s_page == WM_PAGE_CARD) {
            bool has_code = false;
            wm_model_t *m = wm_model_lock();
            if (m) {
                has_code = m->card.saved && m->card.code[0] != '\0';
                wm_model_unlock();
            }
            if (has_code) {
                set_backlight(100);   // 扫码时最大亮度
                enter(ST_QR);
            }
        } else if (ok_click && s_page == WM_PAGE_NOTICES) {
            s_notice_nav = (wm_notice_nav_t) { .view = WM_NOTICE_LIST };
            enter(ST_NOTICES);
        } else if (ok_click && s_page == WM_PAGE_DEVICE) {
            s_menu_kind = MENU_DEVICE;
            s_menu_sel = 0;
            enter(ST_MENU);
        }
        break;
    case ST_NOTICES: {
        if (step && s_notice_nav.view == WM_NOTICE_DETAIL) {
            if (bsp_lvgl_lock(1000)) {
                s_notice_scroll = wm_ui_scroll_notice(step * 60);
                bsp_lvgl_unlock();
            }
            break;
        }
        if (!step && !ok_click && !ok_long) break;
        wm_model_t *m = wm_model_lock();
        if (!m) break;
        uint32_t ids[WM_NOTICE_MAX];
        for (int i = 0; i < m->notice_count; i++) ids[i] = m->notices[i].id;
        wm_notice_view_t before = s_notice_nav.view;
        wm_notice_key(&s_notice_nav, ok_long ? WM_NAV_BACK : ok_click ? WM_NAV_OK : step < 0 ? WM_NAV_UP : WM_NAV_DOWN,
                      ids, m->notice_count);
        if (before == WM_NOTICE_LIST && s_notice_nav.view == WM_NOTICE_DETAIL) {
            s_notice_detail = m->notices[s_notice_nav.selected];
            s_notice_scroll = 0;
        }
        if (m->notice_count > 0) s_seen_notice = m->notices[0].id;
        wm_model_unlock();
        if (s_notice_nav.view == WM_NOTICE_OVERVIEW) enter(ST_HOME);
        else render();
        break;
    }
    case ST_QR:
        if (ok_long) {
            set_backlight(s_brightness);
            enter(ST_HOME);
        }
        break;
    case ST_MENU: {
        int count = s_menu_kind == MENU_DEVICE ? 4 : 2;
        if (step) {
            s_menu_sel = (s_menu_sel + step + count) % count;
            render();
        } else if (ok_click) {
            menu_select();
        } else if (ok_long) {
            enter(rest_state());
        }
        break;
    }
    case ST_MESSAGE:
        enter(s_msg_next);
        break;
    }
}

static void on_net_event(wm_net_state_t state, wm_net_fail_t fail) {
    refresh_sensors();
    if (s_state == ST_CONNECT && s_flow.new_wifi && s_flow.wifi == WM_STEP_ACTIVE) {
        if (state == WM_NET_ONLINE) {
            // 新网络连上了:此刻才把凭据写入 NVS,取代旧网络。
            wm_store_set(WM_KEY_SSID, s_flow.ssid);
            wm_store_set(WM_KEY_PSK, s_flow.password);
            strlcpy(s_ssid, s_flow.ssid, sizeof(s_ssid));
            memset(s_flow.password, 0, sizeof(s_flow.password));
            s_flow.wifi = WM_STEP_DONE;
            if (s_flow.with_bind) {
                start_claim();
                if (s_state == ST_CONNECT) render();
            } else {
                flow_clear();
                show_message(WM_MSG_OK, "网络已连接", s_ssid, rest_state(), true);
            }
        } else if (state == WM_NET_FAILED) {
            const char *why = fail == WM_NET_FAIL_AUTH ? "Wi-Fi 密码不正确,请在 WeMeet 中重新输入后再发送。"
                            : fail == WM_NET_FAIL_NOT_FOUND ? "找不到这个 Wi-Fi。设备只支持 2.4G 网络,请确认名称无误且在信号范围内。"
                            : "无法加入这个 Wi-Fi,请靠近路由器后重试。";
            flow_clear();
            reconnect_saved();
            show_message(WM_MSG_ERROR, "连接失败", why, rest_state(), false);
        }
        return;
    }
    if (state == WM_NET_ONLINE) {
        s_sync_countdown = 0;
        s_pair_countdown = 0;
    }
    // 其余状态只需更新状态栏的信号图标;聆听/连接/提示页不打断。
    if (s_state == ST_HOME || s_state == ST_PAIRING || s_state == ST_WELCOME) render();
}

static uint32_t s_reboot_pending;

// 执行云端下发的一条远程命令,并登记回执。
static void run_remote_command(void) {
    wm_remote_cmd_t cmd = {0};
    wm_model_t *m = wm_model_lock();
    if (!m) return;
    if (m->command.id != 0) {
        cmd = m->command;
        m->ack_id = cmd.id;
        m->command.id = 0;
    }
    wm_model_unlock();
    if (cmd.id == 0) return;
    ESP_LOGI(TAG, "远程命令 #%u: %s", (unsigned)cmd.id, cmd.type);

    if (strcmp(cmd.type, "locate") == 0) {
        s_idle_sec = 0;
        set_backlight(100);
        beep(3);
        show_message(WM_MSG_INFO, "我在这里", "有人在 WeMeet 上查找这台设备。", rest_state(), true);
    } else if (strcmp(cmd.type, "message") == 0 && cmd.text[0]) {
        beep(1);
        show_message(WM_MSG_INFO, "来自 WeMeet", cmd.text, rest_state(), false);
    } else if (strcmp(cmd.type, "brightness") == 0) {
        s_brightness = cmd.value < 20 ? 20 : cmd.value > 100 ? 100 : cmd.value;
        if (s_backlight > BACKLIGHT_DIM) set_backlight(s_brightness);
    } else if (strcmp(cmd.type, "unbind") == 0) {
        wm_cloud_forget();
        s_bound = false;
        show_message(WM_MSG_INFO, "设备已解绑", "这台设备已从账号中移除。", rest_state(), false);
    } else if (strcmp(cmd.type, "reboot") == 0) {
        // Restart only after a successful sync has committed the acknowledgement.
        // A fixed delay can reboot before HTTP finishes and cause a reboot loop.
        s_reboot_pending = cmd.id;
        s_sync_countdown = 0;
    }
}

static void on_sync_ok(void) {
    uint32_t newest = 0;
    wm_model_t *m = wm_model_lock();
    if (m) {
        if (wm_reboot_acknowledged(s_reboot_pending, m->synced_ack_id)) {
            wm_model_unlock();
            esp_restart();
            return;
        }
        if (m->notice_count > 0) newest = m->notices[0].id;
        if (m->next_poll_sec >= 5) s_sync_countdown = m->next_poll_sec;
        wm_model_unlock();
    }
    bool fresh = s_first_sync_done && newest > s_seen_notice;
    if (!s_first_sync_done) {
        // 开机后的第一次同步:已有的通知都当作看过,不然每次开机都会响。
        s_first_sync_done = true;
        s_seen_notice = newest;
    }
    if (s_state == ST_CONNECT) {
        // 理论上不会走到(连接流程不发同步),保险起见直接回主界面。
        flow_clear();
        enter(ST_HOME);
    } else if (s_state == ST_HOME) {
        if (fresh) {
            // 新通知:点亮屏幕、响一声、跳到通知页;用户翻页后才算已读。
            s_idle_sec = 0;
            set_backlight(s_brightness);
            s_page = WM_PAGE_NOTICES;
            render();
            beep(1);
        } else if (s_backlight != 0) {
            render();
        }
    }
    if (s_state == ST_NOTICES && s_backlight != 0) render();
    run_remote_command();
}

static void on_cloud_event(wm_cloud_op_t op, wm_cloud_status_t status) {
    char reason[sizeof(((wm_model_t *)0)->error)] = "";
    wm_model_t *m = wm_model_lock();
    if (m) {
        strlcpy(reason, m->error, sizeof(reason));
        wm_model_unlock();
    }

    switch (op) {
    case WM_CLOUD_OP_CLAIM:
        if (s_state != ST_CONNECT) break;   // 用户已取消
        if (status == WM_CLOUD_OK) {
            s_bound = true;
            flow_clear();
            char owner[sizeof(m->owner)] = "";
            wm_store_get(WM_KEY_OWNER, owner, sizeof(owner));
            beep(2);
            show_message(WM_MSG_OK, "绑定成功", owner[0] ? owner : "设备已加入你的账号", ST_HOME, true);
        } else {
            flow_clear();
            // Wi-Fi 已经保存;绑定没成的话退到配对码页,用户还能用扫码/输码完成绑定。
            const char *why = status == WM_CLOUD_ERR_NETWORK ? "Wi-Fi 已连接,但连不上 WeMeet 服务器。稍后可用配对码完成绑定。"
                            : reason[0] ? reason
                            : "绑定码无效或已过期,请在 WeMeet 重新发起,或使用配对码绑定。";
            show_message(WM_MSG_ERROR, "绑定失败", why, rest_state(), false);
        }
        break;
    case WM_CLOUD_OP_PAIR_START:
        if (s_state != ST_PAIRING) break;
        s_pair_have_code = status == WM_CLOUD_OK;
        s_pair_countdown = s_pair_have_code ? PAIR_POLL_SEC : PAIR_RETRY_SEC;
        render();
        break;
    case WM_CLOUD_OP_PAIR_POLL:
        if (s_state != ST_PAIRING) break;
        if (status == WM_CLOUD_OK) {
            s_bound = true;
            char owner[sizeof(m->owner)] = "";
            wm_store_get(WM_KEY_OWNER, owner, sizeof(owner));
            beep(2);
            show_message(WM_MSG_OK, "绑定成功", owner[0] ? owner : "设备已加入你的账号", ST_HOME, true);
        } else if (status == WM_CLOUD_ERR_REJECTED) {
            s_pair_have_code = false;   // 配对码过期:下一个 tick 重新申请
            s_pair_countdown = 0;
        } else {
            s_pair_countdown = PAIR_POLL_SEC;
        }
        break;
    case WM_CLOUD_OP_SYNC:
        s_sync_busy = false;
        if (status == WM_CLOUD_OK) {
            on_sync_ok();
        } else if (status == WM_CLOUD_ERR_AUTH && s_bound) {
            // 令牌被服务器吊销 = 用户在 PC 端移除了这台设备。
            wm_cloud_forget();
            s_bound = false;
            if (s_state == ST_HOME || s_state == ST_QR || s_state == ST_NOTICES) {
                show_message(WM_MSG_INFO, "设备已解绑", "这台设备已从账号中移除。", rest_state(), false);
            }
        }
        break;
    case WM_CLOUD_OP_UNBIND:
        s_bound = false;
        if (s_reset_pending) {
            factory_reset_local();
        } else {
            show_message(WM_MSG_OK, "已解除绑定", "可以把设备绑定到其他账号了。", rest_state(), true);
        }
        break;
    }
}

// 每秒一次:背光、超时、定时同步、配对轮询、状态栏时钟。
static void on_tick(void) {
    s_idle_sec++;
    s_state_sec++;

    // 背光:聆听/连接/放大二维码时保持常亮,其余状态按无操作时间逐级降低。
    bool keep_on = s_state == ST_LISTEN || s_state == ST_CONNECT || s_state == ST_QR;
    if (keep_on) set_backlight(s_state == ST_QR ? 100 : s_brightness);
    else if (s_idle_sec >= OFF_AFTER_SEC) set_backlight(0);
    else if (s_idle_sec >= DIM_AFTER_SEC) set_backlight(BACKLIGHT_DIM);

    if (s_state == ST_LISTEN && s_state_sec >= LISTEN_TIMEOUT_SEC) {
        enter(s_listen_return);
    } else if (s_state == ST_QR && s_state_sec >= QR_TIMEOUT_SEC) {
        set_backlight(s_brightness);
        enter(ST_HOME);
    } else if (s_state == ST_MESSAGE && s_msg_auto_sec > 0 && --s_msg_auto_sec == 0) {
        enter(s_msg_next);
    }

    bool online = wm_net_state() == WM_NET_ONLINE;

    // 云端同步:已绑定且在线时按间隔进行;息屏后放慢到 4 倍间隔以省电。
    if (s_bound && online && !s_sync_busy && s_state != ST_CONNECT && --s_sync_countdown <= 0) {
        int base = CONFIG_WM_SYNC_INTERVAL_SEC;
        s_sync_countdown = s_backlight == 0 ? base * 4 : base;
        s_sync_busy = wm_cloud_request(WM_CLOUD_OP_SYNC, NULL);
    }

    // 配对码页:没有码就申请,有码就轮询。
    if (s_state == ST_PAIRING && online && --s_pair_countdown <= 0) {
        s_pair_countdown = PAIR_RETRY_SEC;   // 防止结果回来前重复提交
        wm_cloud_request(s_pair_have_code ? WM_CLOUD_OP_PAIR_POLL : WM_CLOUD_OP_PAIR_START, NULL);
    }

    // 状态栏:电量/信号每 30 秒读一次;分钟变化时重绘静态页面。
    bool changed = false;
    if (s_state_sec % BATTERY_PERIOD_SEC == 0) {
        int battery = s_battery, bars = s_wifi_bars;
        refresh_sensors();
        changed = battery != s_battery || bars != s_wifi_bars;
    }
    int minutes = wm_cloud_local_minutes();
    if (minutes != s_minutes) {
        s_minutes = minutes;
        changed = true;
    }
    if (changed && s_backlight != 0 &&
        (s_state == ST_HOME || s_state == ST_PAIRING || s_state == ST_WELCOME)) {
        render();
    }
}

static void app_task(void *arg) {
    (void)arg;
    TickType_t last_tick = xTaskGetTickCount();
    for (;;) {
        event_t ev;
        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - last_tick;
        TickType_t wait = elapsed >= pdMS_TO_TICKS(1000) ? 0 : pdMS_TO_TICKS(1000) - elapsed;
        if (xQueueReceive(s_queue, &ev, wait) == pdTRUE) {
            switch (ev.type) {
            case EV_KEY:
                on_key_event((bsp_btn_t)ev.a, (bsp_btn_ev_t)ev.b);
                break;
            case EV_SONIC_CMD: {
                wm_cmd_t *cmd = ev.ptr;
                if (s_state == ST_LISTEN) handle_command(cmd);
                memset(cmd, 0, sizeof(*cmd));
                free(cmd);
                break;
            }
            case EV_SONIC_FAIL:
                if (s_state != ST_LISTEN) break;
                if (ev.a == WM_LISTEN_EV_AUDIO_ERROR) {
                    show_message(WM_MSG_ERROR, "麦克风异常", "接收已中断,请重试。", s_listen_return, false);
                } else {
                    start_listen(ev.a == WM_LISTEN_EV_BAD_FRAME ? "没听清,请调大音量\n靠近一些再发送一次"
                                                                : "收到了不支持的命令\n请升级设备固件");
                    s_state_sec = 0;
                }
                break;
            case EV_NET:
                on_net_event((wm_net_state_t)ev.a, (wm_net_fail_t)ev.b);
                break;
            case EV_CLOUD:
                on_cloud_event((wm_cloud_op_t)ev.a, (wm_cloud_status_t)ev.b);
                break;
            default:
                break;
            }
        }
        if (xTaskGetTickCount() - last_tick >= pdMS_TO_TICKS(1000)) {
            last_tick += pdMS_TO_TICKS(1000);
            on_tick();
        }
    }
}

esp_err_t wm_app_start(bool audio_ok, bool battery_ok) {
    s_audio_ok = audio_ok;
    s_battery_ok = battery_ok;
    // codec 平时保持休眠,只有聆听和提示音时才唤醒。
    if (audio_ok) bsp_audio_sleep();

    s_queue = xQueueCreate(16, sizeof(event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    esp_err_t err = wm_store_init();
    if (err != ESP_OK) return err;
    wm_store_device_id(s_device_id, sizeof(s_device_id));
    wm_store_get(WM_KEY_SSID, s_ssid, sizeof(s_ssid));

    err = wm_cloud_init(on_cloud);
    if (err != ESP_OK) return err;
    s_bound = wm_cloud_is_bound();
    err = wm_net_init(on_net);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "设备 %s,固件 %s,%s,%s", s_device_id, esp_app_get_description()->version,
             s_ssid[0] ? "已配网" : "未配网", s_bound ? "已绑定" : "未绑定");

    refresh_sensors();
    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;
    wm_ui_init();
    bsp_lvgl_unlock();
    set_backlight(s_brightness);
    enter(rest_state());
    reconnect_saved();   // 有保存的网络就开始连接;没有则保持射频关闭

    // 栈:页面重建(LVGL 建对象 + 二维码编码)与 NVS 写入都在本任务里,8 KB 留足余量。
    if (xTaskCreate(app_task, "wm_app", 8192, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
