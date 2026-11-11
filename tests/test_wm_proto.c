// tests/test_wm_proto.c —— 命令帧解析的 host 测试,重点是对不可信输入的防御。
#include "wm_proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// 往 buf 追加一个 TLV,返回新长度。
static size_t tlv(unsigned char *buf, size_t at, unsigned char tag, const char *value) {
    size_t n = strlen(value);
    buf[at] = tag;
    buf[at + 1] = (unsigned char)n;
    memcpy(buf + at + 2, value, n);
    return at + 2 + n;
}

int main(void) {
    unsigned char p[200];
    wm_cmd_t cmd;
    size_t n;

    // 配网 + 绑定令牌,中间夹一个未知 tag(必须被跳过)。
    p[0] = WM_CMD_PROVISION; p[1] = 0x5A; n = 2;
    n = tlv(p, n, WM_TAG_SSID, "WeMeet-\xE4\xBC\x9A\xE5\x9C\xBA");   // "WeMeet-会场"
    n = tlv(p, n, 0x7F, "future");
    n = tlv(p, n, WM_TAG_PASSWORD, "p@ss w0rd!");
    n = tlv(p, n, WM_TAG_BIND_TOKEN, "K7Q2M9XWTB");
    assert(wm_proto_parse(p, n, &cmd) == WM_PROTO_OK);
    assert(cmd.type == WM_CMD_PROVISION && cmd.seq == 0x5A);
    assert(cmd.has_ssid && strcmp(cmd.ssid, "WeMeet-\xE4\xBC\x9A\xE5\x9C\xBA") == 0);
    assert(cmd.has_password && strcmp(cmd.password, "p@ss w0rd!") == 0);
    assert(cmd.has_token && strcmp(cmd.token, "K7Q2M9XWTB") == 0);
    assert(!cmd.has_url && !cmd.has_text);

    // 开放网络:没有密码字段也合法。
    p[0] = WM_CMD_PROVISION; p[1] = 1; n = tlv(p, 2, WM_TAG_SSID, "Guest");
    assert(wm_proto_parse(p, n, &cmd) == WM_PROTO_OK && !cmd.has_password);

    // 缺 SSID。
    p[0] = WM_CMD_PROVISION; p[1] = 1; n = tlv(p, 2, WM_TAG_PASSWORD, "12345678");
    assert(wm_proto_parse(p, n, &cmd) == WM_PROTO_ERR_MISSING);
    assert(cmd.password[0] == '\0');   // 失败后不残留密码

    // 口令短于 8 位。
    p[0] = WM_CMD_PROVISION; p[1] = 1; n = tlv(p, 2, WM_TAG_SSID, "Home");
    n = tlv(p, n, WM_TAG_PASSWORD, "1234567");
    assert(wm_proto_parse(p, n, &cmd) == WM_PROTO_ERR_FIELD);

    // SSID 超过 32 字节。
    p[0] = WM_CMD_PROVISION; p[1] = 1;
    n = tlv(p, 2, WM_TAG_SSID, "123456789012345678901234567890123");
    assert(wm_proto_parse(p, n, &cmd) == WM_PROTO_ERR_FIELD);

    // TLV 声称的长度超出剩余数据。
    p[0] = WM_CMD_BIND; p[1] = 1; p[2] = WM_TAG_BIND_TOKEN; p[3] = 20; p[4] = 'A';
    assert(wm_proto_parse(p, 5, &cmd) == WM_PROTO_ERR_TLV);
    // 只剩一个 tag 字节。
    assert(wm_proto_parse(p, 3, &cmd) == WM_PROTO_ERR_TLV);

    // 令牌里混入空格 / 控制字符。
    p[0] = WM_CMD_BIND; p[1] = 1; n = tlv(p, 2, WM_TAG_BIND_TOKEN, "AB CD");
    assert(wm_proto_parse(p, n, &cmd) == WM_PROTO_ERR_FIELD);

    // 服务器地址必须带协议头。
    p[0] = WM_CMD_SERVER; p[1] = 1; n = tlv(p, 2, WM_TAG_URL, "https://api.wemeet.example");
    assert(wm_proto_parse(p, n, &cmd) == WM_PROTO_OK && cmd.has_url);
    n = tlv(p, 2, WM_TAG_URL, "ftp://x");
    assert(wm_proto_parse(p, n, &cmd) == WM_PROTO_ERR_FIELD);

    // RESET 不需要任何字段;MESSAGE 需要文本。
    p[0] = WM_CMD_RESET; p[1] = 9;
    assert(wm_proto_parse(p, 2, &cmd) == WM_PROTO_OK && cmd.type == WM_CMD_RESET);
    p[0] = WM_CMD_MESSAGE; p[1] = 9;
    assert(wm_proto_parse(p, 2, &cmd) == WM_PROTO_ERR_MISSING);

    // 未知命令 / 过短。
    p[0] = 0x66; p[1] = 3;
    assert(wm_proto_parse(p, 2, &cmd) == WM_PROTO_ERR_UNKNOWN_CMD && cmd.type == 0x66);
    assert(wm_proto_parse(p, 1, &cmd) == WM_PROTO_ERR_SHORT);
    assert(wm_proto_parse(NULL, 0, &cmd) == WM_PROTO_ERR_SHORT);

    // UTF-8 校验:截断、过长编码、代理区、NUL 都要拒绝。
    assert(wm_utf8_valid((const unsigned char *)"abc\xE4\xB8\xAD", 6));
    assert(wm_utf8_valid((const unsigned char *)"\xF0\x9F\x98\x80", 4));
    assert(!wm_utf8_valid((const unsigned char *)"\xE4\xB8", 2));
    assert(!wm_utf8_valid((const unsigned char *)"\xC0\xAF", 2));
    assert(!wm_utf8_valid((const unsigned char *)"\xED\xA0\x80", 3));
    assert(!wm_utf8_valid((const unsigned char *)"a\0b", 3));
    assert(!wm_utf8_valid((const unsigned char *)"\x80", 1));
    p[0] = WM_CMD_MESSAGE; p[1] = 1; p[2] = WM_TAG_TEXT; p[3] = 2; p[4] = 0xE4; p[5] = 0xB8;
    assert(wm_proto_parse(p, 6, &cmd) == WM_PROTO_ERR_FIELD);

    puts("test_wm_proto: PASS");
    return 0;
}
