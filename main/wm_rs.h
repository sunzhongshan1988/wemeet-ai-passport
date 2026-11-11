// main/wm_rs.h —— GF(2^8) Reed-Solomon 纠错码(纯逻辑,不依赖 ESP-IDF,可 host 测试)。
//
// 声波链路上的错误以"整符号/整字节"成串出现(一次咳嗽、一声关门),RS 码按字节纠错,
// 正好匹配这种突发错误。参数与 H5 侧 web/sonic/wm-sonic.js 必须完全一致:
//   本原多项式 0x11D,生成元 α=2,生成多项式根 α^0 .. α^(npar-1)(fcr = 0)。
#pragma once

#include <stdint.h>

// 单个码字的最大校验字节数。帧头用 4,帧体最多 60(见 wm_sonic.h 的长度规则)。
#define WM_RS_MAX_PARITY 64

// 系统码编码:data[0..len) 不变,把 npar 个校验字节写入 parity[0..npar)。
// 要求 len + npar <= 255,2 <= npar <= WM_RS_MAX_PARITY。不阻塞、无堆分配。
void wm_rs_encode(const uint8_t *data, int len, int npar, uint8_t *parity);

// 就地译码:msg[0..n) = 数据 + 校验(n = 数据长度 + npar)。
// 返回纠正的字节数(>=0);错误超过纠错能力(npar/2 个字节)时返回 -1,
// 此时 msg 内容未定义,调用方必须丢弃。不阻塞、无堆分配,栈占用约 0.7 KB。
int wm_rs_decode(uint8_t *msg, int n, int npar);
