// main/wm_sonic.c —— WeMeet Sonic v1 接收机实现。协议与设计取舍见 wm_sonic.h。
#include "wm_sonic.h"
#include "wm_rs.h"

#include <string.h>

#define RING_MASK (WM_SONIC_RING - 1)

// Goertzel 系数 2·cos(2πf/fs),Q14 定点。顺序:P0, P1, 数据频点 0..15。
// 由 round(2*cos(2*pi*f/16000)*16384) 算出;改频率表时必须同步重算。
static const int32_t s_coeff_pre[2] = { 26510, 24917 };
static const int32_t s_coeff_data[WM_SONIC_TONES] = {
    23170, 21281, 19261, 17121, 14876, 12540, 10126, 7650,
    5126, 2571, 0, -2571, -5126, -7650, -10126, -12540,
};

// Barker-7 前导图样:+1 发 P0,-1 发 P1。自相关旁瓣 ≤ 1,能把同步时刻钉在一个符号内。
static const int8_t s_barker[WM_SONIC_PREAMBLE_LEN] = { 1, 1, 1, -1, -1, 1, -1 };

// 一个符号 = 8 个滑动步。
#define HOPS_PER_SYMBOL (WM_SONIC_SYMBOL / WM_SONIC_HOP)
// 前导相关门限:7 个符号的判别量之和,满分 7·1024。0.72 意味着平均每个符号里
// 目标频率至少占两个前导频率总能量的 86%,随机噪声很难凑出来;漏判的代价只是等发送端重播。
#define SYNC_THRESHOLD ((int32_t)(WM_SONIC_PREAMBLE_LEN * 1024 * 72 / 100))

int wm_sonic_body_parity(int body_len) {
    int npar = 2 * ((body_len + 5) / 6);
    if (npar < 8) npar = 8;
    if (npar > 60) npar = 60;
    return npar;
}

uint16_t wm_sonic_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// 单频点 Goertzel,返回 |X(f)|² · 2^12。
// 定点依据:输入左移 6 位后,状态量峰值约 (N/2)·32768·64 ≈ 5.0e8 < 2^31;
// 系数 Q14 与状态量相乘用 int64 中间量(≤ 3.3e4 · 5e8 ≈ 1.6e13)。
static int64_t goertzel_power(const int16_t *x, int n, int32_t coeff_q14) {
    int32_t s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        int32_t s0 = ((int32_t)x[i] << 6) + (int32_t)(((int64_t)coeff_q14 * s1) >> 14) - s2;
        s2 = s1;
        s1 = s0;
    }
    int64_t cross = (((int64_t)coeff_q14 * s1) >> 14) * (int64_t)s2;
    return (int64_t)s1 * s1 + (int64_t)s2 * s2 - cross;
}

// 把环形缓冲里以 end 为结束时刻的最近 WM_SONIC_WINDOW 个采样拷成线性数组,
// 后面多个频点的 Goertzel 就不用每个采样都做取模。
static void copy_window(const wm_sonic_t *rx, uint32_t end, int16_t *out) {
    uint32_t start = end - WM_SONIC_WINDOW;
    for (int i = 0; i < WM_SONIC_WINDOW; i++) {
        out[i] = rx->ring[(start + (uint32_t)i) & RING_MASK];
    }
}

// 整数平方根(向下取整),只用于 UI 电平显示。
static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

// 回到搜索态。清空判别量历史,避免刚处理完的那段前导在历史里再次触发同步。
static void enter_search(wm_sonic_t *rx) {
    rx->state = WM_SONIC_SEARCH;
    rx->hop_fill = 0;
    rx->confirm_n = 0;
    rx->sym_index = 0;
    rx->sym_total = 0;
    rx->progress = 0;
    memset(rx->d_hist, 0, sizeof(rx->d_hist));
}

void wm_sonic_init(wm_sonic_t *rx) {
    memset(rx, 0, sizeof(*rx));
    enter_search(rx);
}

// 每个滑动步执行一次:算出前导判别量 d 与 Barker 相关值,驱动 SEARCH → CONFIRM → HEADER。
// 返回 true 表示刚刚完成同步(状态已切到 HEADER)。
static int search_hop(wm_sonic_t *rx) {
    int16_t win[WM_SONIC_WINDOW];
    copy_window(rx, rx->count, win);

    int64_t energy = 0;
    for (int i = 0; i < WM_SONIC_WINDOW; i++) energy += (int32_t)win[i] * win[i];
    rx->level = (uint16_t)isqrt64((uint64_t)energy / WM_SONIC_WINDOW);

    int64_t p0 = goertzel_power(win, WM_SONIC_WINDOW, s_coeff_pre[0]);
    int64_t p1 = goertzel_power(win, WM_SONIC_WINDOW, s_coeff_pre[1]);

    // 纯音幅度 A 时 |X|²·2^12 = (A·N/2)²·4096,而 energy = A²·N/2,
    // 所以 "两个前导频率占窗内总能量的比例" = (p0+p1) / (energy · N/2 · 4096)。
    // 占比低于 1/16 时认为没有前导(d = 0),避免在纯噪声里拿两个微小能量的比值瞎判。
    int64_t ref = energy * (WM_SONIC_WINDOW / 2) * 4096;
    int32_t d = 0;
    int64_t sum = p0 + p1;
    if (sum > 0 && sum >= ref / 16) {
        // 先各右移 10 位再相除,避免 (p0-p1)·1024 溢出 int64。
        d = (int32_t)(((p0 - p1) >> 10) * 1024 / ((sum >> 10) + 1));
        if (d > 1024) d = 1024;
        if (d < -1024) d = -1024;
    }
    rx->d_hist[rx->d_pos & 63] = (int16_t)d;
    rx->d_pos++;

    // 与 Barker-7 做相关:最后一个前导符号对应"当前步",往前每个符号隔 8 步。
    int32_t c = 0;
    for (int s = 0; s < WM_SONIC_PREAMBLE_LEN; s++) {
        uint32_t back = (uint32_t)(WM_SONIC_PREAMBLE_LEN - 1 - s) * HOPS_PER_SYMBOL;
        c += s_barker[s] * rx->d_hist[(rx->d_pos - 1 - back) & 63];
    }

    if (rx->state == WM_SONIC_SEARCH) {
        if (c < SYNC_THRESHOLD) return 0;
        rx->state = WM_SONIC_CONFIRM;
        rx->confirm_t0 = rx->count;
        rx->confirm_n = 0;
    }
    // CONFIRM:分析窗完全落在符号内时相关值是个平台(窗比符号短 10 ms)。
    // 多看一个符号时长,取平台中点作为"分析窗居中"的时刻,误差 ≤ 一个滑动步(5 ms)。
    rx->confirm_c[rx->confirm_n++] = c;
    if (rx->confirm_n < (int)(sizeof(rx->confirm_c) / sizeof(rx->confirm_c[0]))) return 0;

    int32_t best = 0;
    for (int i = 0; i < rx->confirm_n; i++) {
        if (rx->confirm_c[i] > best) best = rx->confirm_c[i];
    }
    int first = -1, last = -1;
    for (int i = 0; i < rx->confirm_n; i++) {
        if (rx->confirm_c[i] * 10 >= best * 9) {
            if (first < 0) first = i;
            last = i;
        }
    }
    uint32_t t_star = rx->confirm_t0 + (uint32_t)((first + last + 1) / 2) * WM_SONIC_HOP;
    // t_star 是最后一个前导符号的分析窗结束时刻;下一个符号(帧头首符号)的窗整整晚一个符号。
    rx->next_end = t_star + WM_SONIC_SYMBOL;
    rx->state = WM_SONIC_HEADER;
    rx->sym_index = 0;
    rx->sym_total = 2 * (2 + WM_SONIC_HEADER_PARITY);
    rx->progress = 1;
    return 1;
}

// 解调一个数据符号:16 个频点里能量最大的那个就是这 4 bit。
static uint8_t demod_symbol(wm_sonic_t *rx) {
    int16_t win[WM_SONIC_WINDOW];
    copy_window(rx, rx->next_end, win);

    int64_t energy = 0;
    for (int i = 0; i < WM_SONIC_WINDOW; i++) energy += (int32_t)win[i] * win[i];
    rx->level = (uint16_t)isqrt64((uint64_t)energy / WM_SONIC_WINDOW);

    int best = 0;
    int64_t best_p = -1;
    for (int k = 0; k < WM_SONIC_TONES; k++) {
        int64_t p = goertzel_power(win, WM_SONIC_WINDOW, s_coeff_data[k]);
        if (p > best_p) {
            best_p = p;
            best = k;
        }
    }
    return (uint8_t)best;
}

// 一个块(帧头或帧体)的符号收齐后调用,返回要上报的事件。
static wm_sonic_event_t finish_block(wm_sonic_t *rx) {
    if (rx->state == WM_SONIC_HEADER) {
        int n = 2 + WM_SONIC_HEADER_PARITY;
        if (wm_rs_decode(rx->block, n, WM_SONIC_HEADER_PARITY) < 0 ||
            rx->block[1] != WM_SONIC_HEADER_MAGIC ||
            rx->block[0] < 3 || rx->block[0] > WM_SONIC_MAX_BODY) {
            enter_search(rx);
            return WM_SONIC_EV_BAD_HEADER;
        }
        rx->body_len = rx->block[0];
        rx->state = WM_SONIC_BODY;
        rx->sym_index = 0;
        rx->sym_total = 2 * (rx->body_len + wm_sonic_body_parity(rx->body_len));
        return WM_SONIC_EV_NONE;
    }

    int npar = wm_sonic_body_parity(rx->body_len);
    int fixed = wm_rs_decode(rx->block, rx->body_len + npar, npar);
    int data_len = rx->body_len - 2;
    uint16_t want = (uint16_t)(((uint16_t)rx->block[data_len] << 8) | rx->block[data_len + 1]);
    if (fixed < 0 || wm_sonic_crc16(rx->block, (size_t)data_len) != want) {
        enter_search(rx);
        return WM_SONIC_EV_BAD_BODY;
    }
    memcpy(rx->payload, rx->block, (size_t)data_len);
    rx->payload_len = (uint8_t)data_len;
    rx->corrected = (uint8_t)fixed;
    enter_search(rx);
    rx->progress = 100;
    return WM_SONIC_EV_FRAME;
}

wm_sonic_event_t wm_sonic_feed(wm_sonic_t *rx, const int16_t *pcm, size_t samples) {
    wm_sonic_event_t result = WM_SONIC_EV_NONE;
    for (size_t i = 0; i < samples; i++) {
        rx->ring[rx->count & RING_MASK] = pcm[i];
        rx->count++;

        if (rx->state == WM_SONIC_SEARCH || rx->state == WM_SONIC_CONFIRM) {
            if (++rx->hop_fill < WM_SONIC_HOP) continue;
            rx->hop_fill = 0;
            if (search_hop(rx) && result < WM_SONIC_EV_SYNC) result = WM_SONIC_EV_SYNC;
        }

        // 用带符号差值比较,count 回绕(约 74 小时)时依然正确。
        while ((rx->state == WM_SONIC_HEADER || rx->state == WM_SONIC_BODY) &&
               (int32_t)(rx->count - rx->next_end) >= 0) {
            uint8_t nibble = demod_symbol(rx);
            int byte_index = rx->sym_index / 2;
            if ((rx->sym_index & 1) == 0) {
                rx->block[byte_index] = (uint8_t)(nibble << 4);
            } else {
                rx->block[byte_index] |= nibble;
            }
            rx->sym_index++;
            rx->next_end += WM_SONIC_SYMBOL;
            if (rx->state == WM_SONIC_HEADER) {
                rx->progress = (uint8_t)(1 + rx->sym_index * 9 / rx->sym_total);
            } else {
                rx->progress = (uint8_t)(10 + rx->sym_index * 89 / rx->sym_total);
            }
            if (rx->sym_index >= rx->sym_total) {
                wm_sonic_event_t ev = finish_block(rx);
                if (ev > result) result = ev;
            }
        }
    }
    return result;
}
