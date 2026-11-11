// main/wm_ui.c —— WeMeet Passport 界面实现。线程模型与页面清单见 wm_ui.h。
//
// 实现方式:只有一个常驻根屏幕,每次切页先清空再重建。页面都很小(十几个对象),
// 重建比维护一堆隐藏页面省内存,也不会出现"旧页面的定时器访问已删除对象"的问题——
// 唯一的定时器(聆听页)在清空前统一删除。
#include "wm_ui.h"
#include "wm_navigation.h"
#include "wm_fonts.h"
#include "wm_listen.h"

#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <string.h>

#if !LV_USE_QRCODE
#error "WeMeet Passport needs CONFIG_LV_USE_QRCODE=y. Delete the stale sdkconfig so sdkconfig.defaults is applied again."
#endif

// LVGL 9.3 起枚举改名为 LV_LABEL_LONG_MODE_DOTS,旧名只是兼容宏;兼容宏缺失时在此补上。
#ifndef LV_LABEL_LONG_DOT
#define LV_LABEL_LONG_DOT LV_LABEL_LONG_MODE_DOTS
#endif

// ---- 设计令牌(与 WeMeet Next Web 端 tokens.css 的锌灰体系对应)----
#define C_CANVAS   0xF4F4F5   // 画布底色 zinc-100
#define C_SURFACE  0xFFFFFF   // 卡片
#define C_BORDER   0xE4E4E7   // 描边 zinc-200
#define C_TRACK    0xD4D4D8   // 未激活的图标/进度槽 zinc-300
#define C_INK      0x18181B   // 主文字 zinc-900
#define C_MUTED    0x71717A   // 次要文字 zinc-500
#define C_AI       0x0E7C80   // AI 青(深)
#define C_AI_LIGHT 0x23B6BC   // AI 青(亮)
#define C_AI_GLOW  0x7C5CFF   // AI 光晕紫
#define C_OK       0x16A34A   // 完成/在线
#define C_OK_BG    0xDCFCE7
#define C_WARN     0xC2700A   // 待处理
#define C_WARN_BG  0xFEF3C7
#define C_DANGER   0xDC2626

#define SCREEN_W 240
#define SCREEN_H 320
#define SIDE     18           // 内容左右边距
#define ORB_BASE 84           // 聆听光球的静止直径

static lv_obj_t *s_notice_body;
static uint8_t s_avatar_pixels[WM_AVATAR_BYTES];
static lv_image_dsc_t s_avatar_image;
static lv_obj_t *s_scr;            // 常驻根屏幕
static lv_timer_t *s_listen_timer; // 聆听页的刷新定时器;非聆听页为 NULL
// 聆听页里需要动态更新的对象(仅在 s_listen_timer 存在期间有效)。
static lv_obj_t *s_orb, *s_listen_title, *s_listen_bar;
static int s_orb_size;             // 光球当前直径(做一阶平滑)
static int s_listen_mode = -1;     // 上次显示的是聆听(0)还是接收(1),避免重复设文字

// ---------------------------------------------------------------------------
// 基础构件
// ---------------------------------------------------------------------------

// 没有任何主题样式的纯容器。LVGL 默认主题会给 lv_obj 加内边距、边框和滚动条,
// 这里全部去掉,由调用方按需设置。
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    return obj;
}

// 实心色块(可带圆角)。
static lv_obj_t *fill(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t *obj = box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

// 白色卡片:1 px 描边 + 14 px 圆角。
static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *obj = fill(parent, x, y, w, h, C_SURFACE, 14);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(C_BORDER), 0);
    return obj;
}

// 文字。w > 0 时固定宽度:lines == 1 → 单行,超出显示省略号;lines > 1 → 最多 lines 行。
// w == 0 时宽度随内容。所有文字都显式指定应用字体,不依赖主题默认字体(默认字体没有中文)。
static lv_obj_t *text(lv_obj_t *parent, int x, int y, int w, int lines, const lv_font_t *font,
                      uint32_t color, lv_text_align_t align, const char *str) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    if (w > 0) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, w);
        lv_obj_set_height(label, lines * lv_font_get_line_height(font));
    }
    lv_label_set_text(label, str ? str : "");
    lv_obj_set_pos(label, x, y);
    return label;
}

// 小圆形图标底(对勾 / 叹号 / 序号等),文字居中。
static lv_obj_t *badge_dot(lv_obj_t *parent, int x, int y, int size, uint32_t bg, uint32_t fg,
                           const char *glyph) {
    lv_obj_t *dot = fill(parent, x, y, size, size, bg, LV_RADIUS_CIRCLE);
    lv_obj_t *label = lv_label_create(dot);
    lv_obj_set_style_text_font(label, &wm_font_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg), 0);
    lv_label_set_text(label, glyph);
    lv_obj_center(label);
    return dot;
}

// 二维码。内容过长导致生成失败时返回 NULL(调用方改为显示文字)。
static lv_obj_t *qrcode(lv_obj_t *parent, int size, const char *data) {
    lv_obj_t *qr = lv_qrcode_create(parent);
    lv_qrcode_set_size(qr, size);
    lv_qrcode_set_dark_color(qr, lv_color_hex(C_INK));
    lv_qrcode_set_light_color(qr, lv_color_hex(C_SURFACE));
    if (lv_qrcode_update(qr, data, (uint32_t)strlen(data)) != LV_RESULT_OK) {
        lv_obj_delete(qr);
        return NULL;
    }
    return qr;
}

// 状态栏:品牌固定在左,时间居中,右侧 Wi-Fi 信号格与电池。
static void status_bar(const wm_ui_status_t *st) {
    char clock[16] = "--:--";
    if (st->minutes >= 0) snprintf(clock, sizeof(clock), "%02d:%02d", st->minutes / 60, st->minutes % 60);
    text(s_scr, 22, 9, 68, 1, &wm_font_16, C_INK, LV_TEXT_ALIGN_LEFT, "WeMeet");
    text(s_scr, 92, 9, 56, 1, &wm_font_16, C_INK, LV_TEXT_ALIGN_CENTER, clock);

    int right = SCREEN_W - 26;
    if (st->battery >= 0) {
        // 电池:22×11 外框 + 右侧 2×5 电极;电量低于 20% 变红。
        uint32_t color = st->battery < 20 ? C_DANGER : C_INK;
        int x = right - 24;
        lv_obj_t *shell = box(s_scr, x, 13, 22, 11);
        lv_obj_set_style_bg_color(shell, lv_color_hex(C_SURFACE), 0);
        lv_obj_set_style_bg_opa(shell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(shell, 1, 0);
        lv_obj_set_style_border_color(shell, lv_color_hex(color), 0);
        lv_obj_set_style_radius(shell, 3, 0);
        int w = wm_battery_fill_width(st->battery);
        // Root coordinates avoid the parent border shifting the inner fill.
        if (w > 0) fill(s_scr, x + 3, 16, w, 5, color, 1);
        fill(s_scr, x + 22, 16, 2, 5, color, 1);
        right = x - 8;
    }
    // Wi-Fi:4 根由矮到高的竖条;未联网时全部置灰。
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        bool on = st->wifi_bars > i;
        fill(s_scr, right - 18 + i * 5, 24 - h, 3, h, on ? C_INK : C_TRACK, 1);
    }
}

// 开始一个新页面:停掉聆听定时器,清空屏幕,铺底色并画状态栏。
static void begin(const wm_ui_status_t *st, uint32_t canvas) {
    if (s_listen_timer) {
        lv_timer_delete(s_listen_timer);
        s_listen_timer = NULL;
    }
    s_orb = s_listen_title = s_listen_bar = NULL;
    s_notice_body = NULL;
    lv_obj_clean(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(canvas), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    if (st) status_bar(st);
}

// 底部一行居中的操作提示。
static void hint(const char *str) {
    if (str) text(s_scr, 0, 280, SCREEN_W, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_CENTER, str);
}

// 主界面底部的分页圆点。
static void page_dots(wm_page_t current) {
    int x = (SCREEN_W - (WM_PAGE_COUNT * 6 + (WM_PAGE_COUNT - 1) * 6)) / 2;
    for (int i = 0; i < WM_PAGE_COUNT; i++) {
        fill(s_scr, x + i * 12, 304, 6, 6, i == (int)current ? C_INK : C_TRACK, LV_RADIUS_CIRCLE);
    }
}

// AI 光球:青色渐变 + 紫色光晕。聆听页与引导页共用。
static lv_obj_t *orb(lv_obj_t *parent, int size) {
    lv_obj_t *obj = fill(parent, 0, 0, size, size, C_AI_LIGHT, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(C_AI), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(C_AI_GLOW), 0);
    lv_obj_set_style_shadow_width(obj, size / 3, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_40, 0);
    return obj;
}

void wm_ui_init(void) {
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_CANVAS), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// 引导 / 聆听 / 连接 / 配对
// ---------------------------------------------------------------------------

void wm_ui_show_welcome(const wm_ui_status_t *st) {
    begin(st, C_CANVAS);
    lv_obj_t *mark = orb(s_scr, 36);
    lv_obj_set_pos(mark, SIDE + 4, 48);
    text(s_scr, SIDE + 50, 48, 0, 1, &wm_font_28, C_INK, LV_TEXT_ALIGN_LEFT, "WeMeet");
    text(s_scr, SIDE, 92, SCREEN_W - 2 * SIDE, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_LEFT,
         "你的活动通行证,先连上网络");

    // 三步引导卡片。
    static const char *const steps[3] = {
        "手机打开 WeMeet",
        "进入「设备」添加设备",
        "按 OK,手机靠近设备",
    };
    lv_obj_t *panel = card(s_scr, SIDE, 124, SCREEN_W - 2 * SIDE, 128);
    for (int i = 0; i < 3; i++) {
        char number[2] = { (char)('1' + i), '\0' };
        badge_dot(panel, 12, 14 + i * 36, 24, C_CANVAS, C_INK, number);
        text(panel, 46, 15 + i * 36, 150, 1, &wm_font_16, C_INK, LV_TEXT_ALIGN_LEFT, steps[i]);
    }

    // 主操作:墨黑胶囊(对应 Web 端的黑色主按钮)。
    lv_obj_t *button = fill(s_scr, SIDE, 262, SCREEN_W - 2 * SIDE, 38, C_INK, 19);
    lv_obj_t *label = text(button, 0, 0, 0, 1, &wm_font_16, C_SURFACE, LV_TEXT_ALIGN_CENTER,
                           "按 OK 开始接收");
    lv_obj_center(label);
}

// 聆听页定时器(运行在 LVGL 任务,无需加锁):让光球随输入电平呼吸,并显示收帧进度。
static void listen_tick(lv_timer_t *timer) {
    (void)timer;
    if (!s_orb) return;
    wm_listen_status_t status;
    wm_listen_get(&status);

    // 电平 → 直径:RMS 3000 左右(手机贴近时)顶到最大;一阶平滑避免抖动。
    int level = status.level > 3000 ? 3000 : status.level;
    int target = ORB_BASE + level * 28 / 3000;
    s_orb_size += (target - s_orb_size) / 2;
    lv_obj_set_size(s_orb, s_orb_size, s_orb_size);
    lv_obj_align(s_orb, LV_ALIGN_TOP_MID, 0, 62 + (ORB_BASE + 28 - s_orb_size) / 2);

    int mode = status.receiving ? 1 : 0;
    if (mode != s_listen_mode) {
        s_listen_mode = mode;
        lv_label_set_text(s_listen_title, mode ? "正在接收" : "正在聆听");
        if (mode) lv_obj_remove_flag(s_listen_bar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_listen_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (mode) lv_bar_set_value(s_listen_bar, status.progress, LV_ANIM_OFF);
}

void wm_ui_show_listen(const wm_ui_status_t *st, const char *note) {
    begin(st, C_CANVAS);
    s_orb_size = ORB_BASE;
    s_orb = orb(s_scr, ORB_BASE);
    lv_obj_align(s_orb, LV_ALIGN_TOP_MID, 0, 62 + 14);

    s_listen_title = text(s_scr, 0, 186, SCREEN_W, 1, &wm_font_28, C_INK, LV_TEXT_ALIGN_CENTER,
                          "正在聆听");
    text(s_scr, SIDE, 226, SCREEN_W - 2 * SIDE, 2, &wm_font_16, note ? C_WARN : C_MUTED,
         LV_TEXT_ALIGN_CENTER, note ? note : "把手机扬声器靠近设备\n在 WeMeet 中点击发送");

    // 收帧进度条:平时隐藏,同步上前导后出现。
    s_listen_bar = lv_bar_create(s_scr);
    lv_obj_remove_style_all(s_listen_bar);
    lv_obj_set_pos(s_listen_bar, 50, 274);
    lv_obj_set_size(s_listen_bar, SCREEN_W - 100, 4);
    lv_obj_set_style_radius(s_listen_bar, 2, 0);
    lv_obj_set_style_bg_color(s_listen_bar, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_bg_opa(s_listen_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_listen_bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_listen_bar, lv_color_hex(C_AI), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_listen_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(s_listen_bar, 0, 100);
    lv_obj_add_flag(s_listen_bar, LV_OBJ_FLAG_HIDDEN);

    hint("长按 OK 退出");
    s_listen_mode = -1;
    s_listen_timer = lv_timer_create(listen_tick, 60, NULL);
}

// 连接页里的一行步骤。
static void step_row(lv_obj_t *panel, int row, wm_step_state_t state, const char *label) {
    int y = 14 + row * 40;
    switch (state) {
    case WM_STEP_ACTIVE: {
        lv_obj_t *spinner = lv_spinner_create(panel);
        lv_obj_remove_style_all(spinner);
        lv_obj_set_pos(spinner, 14, y + 2);
        lv_obj_set_size(spinner, 20, 20);
        lv_obj_set_style_arc_width(spinner, 3, LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner, lv_color_hex(C_BORDER), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(spinner, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_width(spinner, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(spinner, lv_color_hex(C_AI), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(spinner, LV_OPA_COVER, LV_PART_INDICATOR);
        break;
    }
    case WM_STEP_DONE:
        badge_dot(panel, 12, y, 24, C_OK_BG, C_OK, "✓");
        break;
    case WM_STEP_FAILED:
        badge_dot(panel, 12, y, 24, C_DANGER, C_SURFACE, "!");
        break;
    default:
        badge_dot(panel, 12, y, 24, C_CANVAS, C_TRACK, "·");
        break;
    }
    text(panel, 48, y + 1, 146, 1, &wm_font_16, state == WM_STEP_PENDING ? C_MUTED : C_INK,
         LV_TEXT_ALIGN_LEFT, label);
}

void wm_ui_show_steps(const wm_ui_status_t *st, const char *detail,
                      wm_step_state_t wifi, wm_step_state_t server, wm_step_state_t bind,
                      bool with_bind) {
    begin(st, C_CANVAS);
    text(s_scr, SIDE, 46, SCREEN_W - 2 * SIDE, 1, &wm_font_28, C_INK, LV_TEXT_ALIGN_LEFT, "正在连接");
    text(s_scr, SIDE, 86, SCREEN_W - 2 * SIDE, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_LEFT, detail);
    int rows = with_bind ? 3 : 2;
    lv_obj_t *panel = card(s_scr, SIDE, 120, SCREEN_W - 2 * SIDE, 12 + rows * 40);
    step_row(panel, 0, wifi, "连接 Wi-Fi");
    step_row(panel, 1, server, "连接 WeMeet 服务器");
    if (with_bind) step_row(panel, 2, bind, "绑定到你的账号");
    hint("长按 OK 取消");
}

void wm_ui_show_pairing(const wm_ui_status_t *st, const char *code, const char *url, int ttl_sec) {
    begin(st, C_CANVAS);
    text(s_scr, SIDE, 40, SCREEN_W - 2 * SIDE, 1, &wm_font_28, C_INK, LV_TEXT_ALIGN_LEFT, "绑定设备");
    lv_obj_t *panel = card(s_scr, SIDE, 82, SCREEN_W - 2 * SIDE, 166);
    if (code == NULL || code[0] == '\0') {
        text(panel, 0, 70, SCREEN_W - 2 * SIDE, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_CENTER,
             "正在获取配对码…");
    } else {
        lv_obj_t *qr = (url && url[0]) ? qrcode(panel, 104, url) : NULL;
        if (qr) lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_t *digits = text(panel, 0, qr ? 120 : 60, SCREEN_W - 2 * SIDE, 1, &wm_font_28, C_INK,
                                LV_TEXT_ALIGN_CENTER, code);
        lv_obj_set_style_text_letter_space(digits, 4, 0);
    }
    char line[64];
    if (ttl_sec > 0) snprintf(line, sizeof(line), "扫码或在 WeMeet 输入配对码\n%d 分钟内有效", (ttl_sec + 59) / 60);
    else snprintf(line, sizeof(line), "扫码或在 WeMeet 输入配对码");
    text(s_scr, SIDE, 254, SCREEN_W - 2 * SIDE, 2, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_CENTER, line);
}

// ---------------------------------------------------------------------------
// 主界面四个分页
// ---------------------------------------------------------------------------

void wm_ui_show_card(const wm_ui_status_t *st, const wm_model_t *model) {
    begin(st, C_CANVAS);
    const wm_card_t *c = &model->card;
    text(s_scr, SIDE, 40, SCREEN_W - 2 * SIDE, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_LEFT, "我的名片");
    lv_obj_t *panel = card(s_scr, SIDE, 72, SCREEN_W - 2 * SIDE, 196);
    if (c->avatar_ready) {
        // LVGL may draw after this returns; cloud updates cannot mutate these pixels.
        memcpy(s_avatar_pixels, c->avatar, WM_AVATAR_BYTES);
        lv_image_cache_drop(&s_avatar_image);
        s_avatar_image = (lv_image_dsc_t) {
            .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                        .w = WM_AVATAR_SIDE, .h = WM_AVATAR_SIDE, .stride = WM_AVATAR_SIDE * 2 },
            .data_size = WM_AVATAR_BYTES, .data = s_avatar_pixels,
        };
        lv_obj_t *portrait = lv_image_create(panel);
        lv_image_set_src(portrait, &s_avatar_image);
        lv_obj_set_pos(portrait, 77, 16);
    } else {
        lv_obj_t *portrait = fill(panel, 77, 16, 48, 48, C_CANVAS, 12);
        fill(portrait, 17, 8, 14, 14, C_TRACK, LV_RADIUS_CIRCLE);
        fill(portrait, 10, 26, 28, 16, C_TRACK, 8);
    }
    text(panel, 10, 77, 182, 1, &wm_font_28, C_INK, LV_TEXT_ALIGN_CENTER,
         c->name[0] ? c->name : model->owner);
    text(panel, 12, 116, 178, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_CENTER, c->title);
    text(panel, 12, 143, 178, 2, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_CENTER, c->org);
    hint(c->code[0] ? "OK 展示名片码" : "请在 WeMeet 完善名片");
    page_dots(WM_PAGE_CARD);
}

void wm_ui_show_qr(const char *code, const char *caption) {
    begin(NULL, C_SURFACE);
    lv_obj_t *qr = qrcode(s_scr, 196, code);
    if (qr) lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 34);
    text(s_scr, SIDE, 242, SCREEN_W - 2 * SIDE, 1, &wm_font_16, C_INK, LV_TEXT_ALIGN_CENTER, caption);
    text(s_scr, 0, 272, SCREEN_W, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_CENTER, "长按 OK 返回");
}

// 分页标题 + 空状态说明。返回 true 表示已画空状态,调用方不必再画列表。
static bool page_header(const char *title, bool empty, const char *empty_text) {
    text(s_scr, SIDE, 40, SCREEN_W - 2 * SIDE, 1, &wm_font_28, C_INK, LV_TEXT_ALIGN_LEFT, title);
    if (!empty) return false;
    text(s_scr, SIDE, 150, SCREEN_W - 2 * SIDE, 2, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_CENTER, empty_text);
    return true;
}

void wm_ui_show_agenda(const wm_ui_status_t *st, const wm_model_t *model) {
    begin(st, C_CANVAS);
    page_dots(WM_PAGE_AGENDA);
    if (page_header("日程", model->agenda_count == 0, "暂无日程")) return;
    // 每行 42 px:左侧时间(AI 青,等同 Web 端的时间列),右侧标题 + 地点。
    for (int i = 0; i < model->agenda_count && i < WM_AGENDA_MAX; i++) {
        const wm_agenda_item_t *a = &model->agenda[i];
        int y = 84 + i * 42;
        if (i > 0) fill(s_scr, SIDE, y - 3, SCREEN_W - 2 * SIDE, 1, C_BORDER, 0);
        text(s_scr, SIDE, y, 50, 1, &wm_font_16, C_AI, LV_TEXT_ALIGN_LEFT, a->time);
        text(s_scr, SIDE + 54, y, SCREEN_W - 2 * SIDE - 54, 1, &wm_font_16, C_INK, LV_TEXT_ALIGN_LEFT, a->title);
        text(s_scr, SIDE + 54, y + 19, SCREEN_W - 2 * SIDE - 54, 1, &wm_font_16, C_MUTED,
             LV_TEXT_ALIGN_LEFT, a->place);
    }
}

void wm_ui_show_notices(const wm_ui_status_t *st, const wm_model_t *model, bool has_unread) {
    begin(st, C_CANVAS);
    page_dots(WM_PAGE_NOTICES);
    if (page_header("通知", model->notice_count == 0, "暂无通知")) return;
    if (has_unread) fill(s_scr, SIDE + 64, 54, 8, 8, C_WARN, LV_RADIUS_CIRCLE);
    for (int i = 0; i < model->notice_count && i < 3; i++) {
        const wm_notice_t *n = &model->notices[i];
        lv_obj_t *panel = card(s_scr, SIDE, 82 + i * 64, SCREEN_W - 2 * SIDE, 58);
        text(panel, 10, 7, 182, 1, &wm_font_16, C_INK, LV_TEXT_ALIGN_LEFT, n->title);
        text(panel, 10, 30, 182, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_LEFT, n->body);
    }
    hint("OK 查看通知");
}

void wm_ui_show_notice_list(const wm_ui_status_t *st, const wm_model_t *model, int selected) {
    begin(st, C_CANVAS);
    if (page_header("通知列表", model->notice_count == 0, "暂无通知")) { hint("长按 OK 返回"); return; }
    char count[16];
    snprintf(count, sizeof(count), "%d/%d", selected + 1, model->notice_count);
    text(s_scr, 172, 48, 48, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_RIGHT, count);
    for (int i = 0; i < model->notice_count; i++) {
        lv_obj_t *row = card(s_scr, SIDE, 84 + i * 46, 204, 40);
        if (i == selected) {
            lv_obj_set_style_border_color(row, lv_color_hex(C_INK), 0);
            fill(row, 0, 11, 3, 16, C_INK, 1);
        }
        text(row, 12, 9, 178, 1, &wm_font_16, C_INK, LV_TEXT_ALIGN_LEFT, model->notices[i].title);
    }
    hint("OK 详情 · 长按返回");
}

void wm_ui_show_notice_detail(const wm_ui_status_t *st, const wm_notice_t *notice, int scroll) {
    begin(st, C_CANVAS);
    text(s_scr, SIDE, 40, 204, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_LEFT, "通知详情");
    s_notice_body = box(s_scr, SIDE, 76, 204, 198);
    lv_obj_add_flag(s_notice_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_notice_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_notice_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *title = text(s_notice_body, 0, 0, 204, 1, &wm_font_16, C_INK, LV_TEXT_ALIGN_LEFT, notice->title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(title, LV_SIZE_CONTENT);
    lv_obj_update_layout(title);
    int y = lv_obj_get_height(title) + 14;
    lv_obj_t *body = text(s_notice_body, 0, y, 204, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_LEFT,
                          notice->body[0] ? notice->body : "暂无更多内容");
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_obj_set_style_text_line_space(body, 5, 0);
    lv_obj_update_layout(s_notice_body);
    lv_obj_scroll_to_y(s_notice_body, scroll, LV_ANIM_OFF);
    hint("上下滚动 · 长按返回");
}

int wm_ui_scroll_notice(int delta) {
    if (!s_notice_body) return 0;
    lv_obj_scroll_by_bounded(s_notice_body, 0, -delta, LV_ANIM_OFF);
    return lv_obj_get_scroll_y(s_notice_body);
}

void wm_ui_show_device(const wm_ui_status_t *st, const wm_ui_device_t *info) {
    begin(st, C_CANVAS);
    page_dots(WM_PAGE_DEVICE);
    page_header("设备", false, NULL);
    const char *keys[4] = { "设备号", "账号", "网络", "固件" };
    const char *values[4] = {
        info->device_id,
        info->owner[0] ? info->owner : "未绑定",
        info->ssid[0] ? info->ssid : "未配置",
        info->firmware,
    };
    lv_obj_t *panel = card(s_scr, SIDE, 84, SCREEN_W - 2 * SIDE, 4 * 40 + 4);
    for (int i = 0; i < 4; i++) {
        int y = 12 + i * 40;
        if (i > 0) fill(panel, 12, y - 10, SCREEN_W - 2 * SIDE - 24, 1, C_BORDER, 0);
        text(panel, 12, y, 52, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_LEFT, keys[i]);
        // 设备号较长:去掉 "WM-" 前缀后的 12 位十六进制正好放得下。
        const char *value = values[i];
        if (i == 0 && strncmp(value, "WM-", 3) == 0) value += 3;
        text(panel, 66, y, SCREEN_W - 2 * SIDE - 78 - (i == 2 ? 14 : 0), 1, &wm_font_16, C_INK,
             LV_TEXT_ALIGN_RIGHT, value);
        // 网络行末尾的在线状态点(进行中绿 / 灰)。
        if (i == 2) {
            fill(panel, SCREEN_W - 2 * SIDE - 22, y + 6, 8, 8, info->online ? C_OK : C_TRACK,
                 LV_RADIUS_CIRCLE);
        }
    }
    text(s_scr, 0, 262, SCREEN_W, 1, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_CENTER, "按 OK 打开设备菜单");
}

// ---------------------------------------------------------------------------
// 菜单 / 提示
// ---------------------------------------------------------------------------

void wm_ui_show_menu(const wm_ui_status_t *st, const char *title, const char *const *items,
                     int count, int selected) {
    begin(st, C_CANVAS);
    text(s_scr, SIDE, 40, SCREEN_W - 2 * SIDE, 1, &wm_font_28, C_INK, LV_TEXT_ALIGN_LEFT, title);
    for (int i = 0; i < count && i < 6; i++) {
        bool on = i == selected;
        // 选中项:墨黑底白字(与 Web 端命令面板的选中态一致);其余为白卡片。
        lv_obj_t *row = on ? fill(s_scr, SIDE, 84 + i * 36, SCREEN_W - 2 * SIDE, 32, C_INK, 10)
                           : card(s_scr, SIDE, 84 + i * 36, SCREEN_W - 2 * SIDE, 32);
        lv_obj_set_style_radius(row, 10, 0);
        text(row, 14, 5, SCREEN_W - 2 * SIDE - 28, 1, &wm_font_16, on ? C_SURFACE : C_INK,
             LV_TEXT_ALIGN_LEFT, items[i]);
    }
}

void wm_ui_show_message(const wm_ui_status_t *st, wm_msg_kind_t kind, const char *title,
                        const char *body, const char *hint_text) {
    begin(st, C_CANVAS);
    uint32_t bg = kind == WM_MSG_OK ? C_OK_BG : kind == WM_MSG_ERROR ? C_DANGER : C_AI;
    uint32_t fg = kind == WM_MSG_OK ? C_OK : C_SURFACE;
    const char *glyph = kind == WM_MSG_OK ? "✓" : kind == WM_MSG_ERROR ? "!" : "i";
    lv_obj_t *icon = badge_dot(s_scr, 0, 0, 52, bg, fg, glyph);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_text_font(lv_obj_get_child(icon, 0), &wm_font_28, 0);
    text(s_scr, 10, 132, SCREEN_W - 20, 1, &wm_font_28, C_INK, LV_TEXT_ALIGN_CENTER, title);
    text(s_scr, SIDE, 176, SCREEN_W - 2 * SIDE, 4, &wm_font_16, C_MUTED, LV_TEXT_ALIGN_CENTER, body);
    hint(hint_text);
}
