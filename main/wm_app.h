// main/wm_app.h —— WeMeet Passport 应用层:状态机、按键分发、配网/绑定/同步流程。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"
#include <stdbool.h>

// 启动应用任务并显示首屏。要求显示/LVGL 已初始化。
// audio_ok / battery_ok 是对应 BSP 外设的初始化结果:失败时相关功能降级而不是崩溃
// (无麦克风 → 不能声波配网,仍可显示;无电量计 → 隐藏电池图标)。
esp_err_t wm_app_start(bool audio_ok, bool battery_ok);

// 投递一个按键事件。可在按键回调(esp_timer 任务)里调用:只入队,不阻塞。
void wm_app_post_key(bsp_btn_t btn, bsp_btn_ev_t event);
