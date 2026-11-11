// main/wm_proto.c —— WeMeet 设备命令帧解析。格式与约定见 wm_proto.h。
#include "wm_proto.h"

#include <string.h>

bool wm_utf8_valid(const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i];
        size_t need;
        uint32_t cp;
        if (c == 0) return false;                 // NUL 会截断 C 字符串,一律拒绝
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
        else return false;                        // 孤立的后续字节或非法首字节
        if (i + need >= len) return false;        // 被截断:后续字节不够
        for (size_t k = 1; k <= need; k++) {
            uint8_t t = data[i + k];
            if ((t & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (t & 0x3F);
        }
        // 拒绝过长编码(同一码点必须用最短形式)、UTF-16 代理区和超出 Unicode 范围的值。
        if (need == 1 && cp < 0x80) return false;
        if (need == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
        if (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
        i += need + 1;
    }
    return true;
}

// 是否全部为可打印 ASCII(0x21..0x7E,不含空格)。令牌和 URL 用它校验。
static bool ascii_printable(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] < 0x21 || data[i] > 0x7E) return false;
    }
    return true;
}

// 把一个 TLV 值拷进定长字符串字段。超长或字符集不符返回 false。
static bool take(char *dst, size_t max, bool *has, const uint8_t *value, size_t len,
                 size_t min, bool ascii) {
    if (len < min || len > max) return false;
    if (ascii ? !ascii_printable(value, len) : !wm_utf8_valid(value, len)) return false;
    memcpy(dst, value, len);
    dst[len] = '\0';
    *has = true;
    return true;
}

wm_proto_err_t wm_proto_parse(const uint8_t *payload, size_t len, wm_cmd_t *out) {
    memset(out, 0, sizeof(*out));
    if (payload == NULL || len < 2) return WM_PROTO_ERR_SHORT;
    out->type = payload[0];
    out->seq = payload[1];

    wm_proto_err_t err = WM_PROTO_OK;
    size_t i = 2;
    while (i < len && err == WM_PROTO_OK) {
        if (i + 2 > len) { err = WM_PROTO_ERR_TLV; break; }
        uint8_t tag = payload[i];
        size_t vlen = payload[i + 1];
        const uint8_t *value = payload + i + 2;
        if (i + 2 + vlen > len) { err = WM_PROTO_ERR_TLV; break; }
        bool ok = true;
        switch (tag) {
        case WM_TAG_SSID:
            ok = take(out->ssid, WM_SSID_MAX, &out->has_ssid, value, vlen, 1, false);
            break;
        case WM_TAG_PASSWORD:
            ok = take(out->password, WM_PASSWORD_MAX, &out->has_password, value, vlen, 0, false);
            break;
        case WM_TAG_BIND_TOKEN:
            ok = take(out->token, WM_TOKEN_MAX, &out->has_token, value, vlen, 1, true);
            break;
        case WM_TAG_URL:
            ok = take(out->url, WM_URL_MAX, &out->has_url, value, vlen, 1, true) &&
                 (strncmp(out->url, "https://", 8) == 0 || strncmp(out->url, "http://", 7) == 0);
            break;
        case WM_TAG_TEXT:
            ok = take(out->text, WM_TEXT_MAX, &out->has_text, value, vlen, 1, false);
            break;
        default:
            break;   // 未知 tag:向前兼容,跳过
        }
        if (!ok) err = WM_PROTO_ERR_FIELD;
        i += 2 + vlen;
    }

    if (err == WM_PROTO_OK) {
        switch (out->type) {
        case WM_CMD_PROVISION:
            if (!out->has_ssid) err = WM_PROTO_ERR_MISSING;
            // WPA2 口令至少 8 个字符;给了口令却短于 8 一定连不上,提前拒绝比超时友好。
            else if (out->has_password && out->password[0] && strlen(out->password) < 8) {
                err = WM_PROTO_ERR_FIELD;
            }
            break;
        case WM_CMD_BIND:
            if (!out->has_token) err = WM_PROTO_ERR_MISSING;
            break;
        case WM_CMD_SERVER:
            if (!out->has_url) err = WM_PROTO_ERR_MISSING;
            break;
        case WM_CMD_RESET:
            break;
        case WM_CMD_MESSAGE:
            if (!out->has_text) err = WM_PROTO_ERR_MISSING;
            break;
        default:
            err = WM_PROTO_ERR_UNKNOWN_CMD;
            break;
        }
    }
    if (err != WM_PROTO_OK) {
        uint8_t type = out->type, seq = out->seq;
        memset(out, 0, sizeof(*out));
        // 保留 type/seq 方便调用方记录"收到了哪种不支持的命令"。
        out->type = type;
        out->seq = seq;
    }
    return err;
}
