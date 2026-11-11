// main/main.c —— WeMeet Passport 固件入口:板级外设初始化 + 按键回调,然后把控制权交给
// 应用层(wm_app.c)。页面、状态机、联网逻辑都不在这里。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "key_guard.h"
#include "wm_app.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include <stdbool.h>

static const char *TAG = "main";

// 应用层事件队列就绪后置 true;在此之前到达的按键直接丢弃。
static volatile bool s_input_ready;

// 深睡按键唤醒后,那颗键在启动期间还按着:按键组件晚几百毫秒才开始观测,会把这次
// "已经按下"的电平判成长按并立刻触发动作。守卫在唤醒键松手前吞掉所有按键事件;
// 兜底超时只为防止按键卡住导致键盘永久失效。状态机在 key_guard.h(有 host 测试)。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// 按键回调。运行在 button 组件共用的 esp_timer 任务:只做守卫判断和入队,立即返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // 守卫必须先于就绪检查:松手事件可能在"初始化后采样电压"与"输入就绪"之间到达,
    // 若在这里被就绪检查丢掉,守卫就会一直武装着吞掉下一次真实按键。
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready) return;
    wm_app_post_key(btn, ev);
}

void app_main(void) {
    ESP_LOGI(TAG, "WeMeet Passport 启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    bsp_i2c_init();

    // 屏幕是唯一的输出设备,失败就没有任何界面可言:打清楚日志后停在这里。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    // 其余外设单项失败不阻塞启动:应用层按可用性降级。
    const esp_err_t button_err = bsp_button_init(on_key, NULL);
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
    } else if (s_key_guard.armed) {
        // 若唤醒键在按键驱动启动前就已松手,组件不会产生 release 事件;按真实电压同步一次。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }
    const bool audio_ok = bsp_audio_init() == ESP_OK;
    const bool battery_ok = bsp_battery_init() == ESP_OK;
    if (!audio_ok) ESP_LOGE(TAG, "音频初始化失败:声波配网与提示音不可用");
    if (!battery_ok) ESP_LOGW(TAG, "电量计初始化失败:不显示电量");

    const esp_err_t app_err = wm_app_start(audio_ok, battery_ok);
    if (app_err != ESP_OK) {
        ESP_LOGE(TAG, "应用启动失败: %s", esp_err_to_name(app_err));
        return;
    }
    s_input_ready = true;
    ESP_LOGI(TAG, "就绪:Button=%d Audio=%d Battery=%d", button_err == ESP_OK, audio_ok, battery_ok);
}
