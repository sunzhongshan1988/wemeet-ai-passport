// main/wm_store.c —— NVS 持久化实现。约定见 wm_store.h。
#include "wm_store.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "wm_store";
#define NS "wm"

esp_err_t wm_store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 这两种情况下 NVS 无法挂载,保留旧数据也读不出来;擦除后设备回到开箱状态。
        ESP_LOGW(TAG, "NVS 需要重建(%s),配网与绑定信息将丢失", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    return err;
}

bool wm_store_get(const char *key, char *out, size_t cap) {
    if (cap == 0) return false;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

esp_err_t wm_store_set(const char *key, const char *value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (value == NULL || value[0] == '\0') {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   // 本来就没有,等同于删除成功
    } else {
        err = nvs_set_str(h, key, value);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "写入 %s 失败: %s", key, esp_err_to_name(err));
    return err;
}

void wm_store_device_id(char *out, size_t cap) {
    uint8_t mac[6] = {0};
    // 读出厂 eFuse MAC;失败时 mac 保持全 0,设备号仍然格式合法,只是不唯一。
    if (esp_efuse_mac_get_default(mac) != ESP_OK) ESP_LOGE(TAG, "读取出厂 MAC 失败");
    snprintf(out, cap, "WM-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void wm_store_secret(char *out, size_t cap) {
    if (wm_store_get(WM_KEY_SECRET, out, cap) && strlen(out) == 32) return;
    uint8_t raw[16];
    // 注意:射频未启动时 esp_fill_random 的熵较弱。本函数只在联网后的云端请求里调用,
    // 那时 Wi-Fi 已启动,硬件随机数是真随机。
    esp_fill_random(raw, sizeof(raw));
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", raw[i]);
    wm_store_set(WM_KEY_SECRET, hex);
    snprintf(out, cap, "%s", hex);
}

void wm_store_api_base(char *out, size_t cap) {
    if (!wm_store_get(WM_KEY_API, out, cap)) snprintf(out, cap, "%s", CONFIG_WM_API_BASE);
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/') out[--n] = '\0';
}
