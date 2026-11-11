// main/wm_proto.h —— WeMeet 设备命令帧解析(纯逻辑,可 host 测试)。
//
// 命令帧是声波链路(wm_sonic)一帧的 payload,以后也可以走 BLE / 云端下发,格式不变:
//   payload = [cmd:1][seq:1][TLV...]     TLV = [tag:1][len:1][value:len]
// 设计要点:
//   - 不认识的 tag 直接跳过 → 新 H5 给旧固件多发字段不会失败;
//   - 不认识的 cmd 报错 → 设备提示"需要升级固件",而不是悄悄做错事;
//   - seq 由发送端随机生成,H5 会把同一帧重播几遍,设备靠 (cmd, seq) 去重。
// H5 侧的编码实现在 web/sonic/wm-proto.js,两边的常量必须一致。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 命令类型(payload 第 1 字节)。
typedef enum {
    WM_CMD_PROVISION = 0x01,  // 配网;带绑定令牌时联网后自动向服务器认领
    WM_CMD_BIND      = 0x02,  // 仅绑定:设备已联网,只下发绑定令牌
    WM_CMD_SERVER    = 0x03,  // 设置服务器地址(联调 / 私有化部署)
    WM_CMD_RESET     = 0x04,  // 清除配网与绑定(设备端仍需按键确认)
    WM_CMD_MESSAGE   = 0x05,  // 在屏幕上显示一条文本(联调 / 找设备)
} wm_cmd_type_t;

// TLV 标签。
typedef enum {
    WM_TAG_SSID       = 0x01,  // UTF-8,1..32 字节
    WM_TAG_PASSWORD   = 0x02,  // UTF-8,0..64 字节;开放网络可省略
    WM_TAG_BIND_TOKEN = 0x03,  // 可打印 ASCII,1..24 字节
    WM_TAG_URL        = 0x04,  // 可打印 ASCII,1..96 字节,必须以 http:// 或 https:// 开头
    WM_TAG_TEXT       = 0x05,  // UTF-8,1..96 字节
} wm_tag_t;

#define WM_SSID_MAX     32
#define WM_PASSWORD_MAX 64
#define WM_TOKEN_MAX    24
#define WM_URL_MAX      96
#define WM_TEXT_MAX     96

// 解析结果。字符串字段都以 NUL 结尾;某字段未出现时对应 has_* 为 false 且字符串为空。
// 结构体约 330 字节且含 Wi-Fi 密码:用完后请 memset 清零,不要打印 password。
typedef struct {
    uint8_t type;                         // wm_cmd_type_t
    uint8_t seq;                          // 发送端随机序号,用于重播去重
    bool has_ssid, has_password, has_token, has_url, has_text;
    char ssid[WM_SSID_MAX + 1];
    char password[WM_PASSWORD_MAX + 1];
    char token[WM_TOKEN_MAX + 1];
    char url[WM_URL_MAX + 1];
    char text[WM_TEXT_MAX + 1];
} wm_cmd_t;

typedef enum {
    WM_PROTO_OK = 0,
    WM_PROTO_ERR_SHORT,        // 不足 2 字节,连 cmd/seq 都没有
    WM_PROTO_ERR_TLV,          // TLV 越界(len 超出剩余数据)
    WM_PROTO_ERR_FIELD,        // 字段长度或字符集不合法
    WM_PROTO_ERR_UNKNOWN_CMD,  // 不认识的命令类型:固件比 H5 旧
    WM_PROTO_ERR_MISSING,      // 该命令必需的字段缺失
} wm_proto_err_t;

// 解析一条命令。成功返回 WM_PROTO_OK 并填充 out;失败时 out 除 type/seq 外全部清零
// (保留 type/seq 便于提示"收到了哪种不支持的命令")。
// 不阻塞、无堆分配。payload 可以来自任意不可信来源(声波谁都能播),函数对所有输入都安全。
wm_proto_err_t wm_proto_parse(const uint8_t *payload, size_t len, wm_cmd_t *out);

// 校验一段字节是否为合法 UTF-8(拒绝截断、过长编码、代理区和 NUL)。
// 屏幕上显示任何外来文本前都要先过这一关,否则 LVGL 会画出乱码。
bool wm_utf8_valid(const uint8_t *data, size_t len);
