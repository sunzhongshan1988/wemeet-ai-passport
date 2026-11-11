// main/wm_store.h —— 设备持久化配置(NVS 命名空间 "wm")。
// 保存 Wi-Fi 凭据、服务器地址、云端签发的设备令牌等。所有接口线程安全(NVS 自带锁),
// 但会访问 Flash:不要在按键回调或 LVGL 任务里调用写接口。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// 键名(NVS 限 15 字符)。
#define WM_KEY_SSID   "ssid"     // Wi-Fi 名称
#define WM_KEY_PSK    "psk"      // Wi-Fi 密码(NVS 未加密:量产前应启用 Flash/NVS 加密)
#define WM_KEY_API    "api"      // 服务器地址覆盖值;不存在时用 CONFIG_WM_API_BASE
#define WM_KEY_TOKEN  "token"    // 云端签发的设备令牌;存在即"已绑定"
#define WM_KEY_SECRET "secret"   // 设备自生成的随机密钥(32 位十六进制),配对码流程用
#define WM_KEY_OWNER  "owner"    // 绑定账号显示名(离线时也能显示)

// 初始化 NVS。分区无可用页或版本不兼容时擦除后重建(此时配网与绑定信息丢失,
// 设备回到开箱状态);其他错误原样返回。必须在任何 get/set 之前调用一次。
esp_err_t wm_store_init(void);

// 读字符串。键不存在、缓冲不够或读失败都返回 false 且 out 置为空串。
bool wm_store_get(const char *key, char *out, size_t cap);

// 写字符串;value 为 NULL 或空串时删除该键。立即提交。
esp_err_t wm_store_set(const char *key, const char *value);

// 设备号:由出厂 MAC 派生,形如 "WM-A1B2C3D4E5F6"。out 至少 16 字节。永不失败。
void wm_store_device_id(char *out, size_t cap);

// 设备密钥:首次调用时用硬件随机数生成并保存,之后保持不变(恢复出厂也保留,
// 这样服务器能认出是同一台设备)。out 至少 33 字节。
void wm_store_secret(char *out, size_t cap);

// 当前服务器地址(不带结尾斜杠)。out 至少 97 字节。
void wm_store_api_base(char *out, size_t cap);
