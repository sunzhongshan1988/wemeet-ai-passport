// tests/test_wm_sonic.c —— 声波接收机的 host 测试。
// 测试内置一个浮点参考调制器(与 web/sonic/wm-sonic.js 同一套规则),再叠加真实链路上
// 会遇到的损伤:任意起始相位、宽带噪声、小信号量化、房间回声、收发时钟偏差、突发遮挡。
#include "wm_rs.h"
#include "wm_sonic.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

static unsigned s_seed = 0x534F4E49u;
static unsigned rnd(void) {
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}
// 近似高斯噪声(12 个均匀数之和),方差 1。
static double gauss(void) {
    double s = 0;
    for (int i = 0; i < 12; i++) s += (double)(rnd() & 0xFFFF) / 65536.0;
    return s - 6.0;
}

typedef struct {
    double amplitude;    // 信号峰值(int16 量纲)
    double noise_rms;    // 叠加的宽带噪声 RMS
    double skew_ppm;     // 发送端时钟相对接收端的偏差
    double echo_gain;    // 回声幅度(0 = 无),延迟固定 12 ms
    int lead;            // 帧前的静音/噪声采样数
    int burst_at;        // >=0:从该采样起清零 burst_len 个采样(模拟遮挡)
    int burst_len;
} channel_t;

// 调制一帧并经过信道,返回 malloc 的 PCM 与长度。
static int16_t *modulate(const uint8_t *payload, int plen, const channel_t *ch, size_t *out_n) {
    uint8_t bytes[6 + 255];
    int body = plen + 2;
    bytes[0] = (uint8_t)body;
    bytes[1] = WM_SONIC_HEADER_MAGIC;
    wm_rs_encode(bytes, 2, WM_SONIC_HEADER_PARITY, bytes + 2);
    uint8_t *b = bytes + 6;
    memcpy(b, payload, (size_t)plen);
    uint16_t crc = wm_sonic_crc16(payload, (size_t)plen);
    b[plen] = (uint8_t)(crc >> 8);
    b[plen + 1] = (uint8_t)crc;
    int npar = wm_sonic_body_parity(body);
    wm_rs_encode(b, body, npar, b + body);
    int nbytes = 6 + body + npar;

    static const int barker[7] = {1, 1, 1, -1, -1, 1, -1};
    int nsym = 7 + nbytes * 2;
    double *freq = malloc(sizeof(double) * (size_t)nsym);
    for (int i = 0; i < 7; i++) freq[i] = barker[i] > 0 ? WM_SONIC_F_P0 : WM_SONIC_F_P1;
    for (int i = 0; i < nbytes; i++) {
        freq[7 + 2 * i] = WM_SONIC_F_DATA0 + WM_SONIC_F_STEP * (bytes[i] >> 4);
        freq[8 + 2 * i] = WM_SONIC_F_DATA0 + WM_SONIC_F_STEP * (bytes[i] & 15);
    }

    // 发送端时钟快 skew → 接收端看到的符号更短、频率更高。
    double scale = 1.0 + ch->skew_ppm * 1e-6;
    double sym = WM_SONIC_SYMBOL / scale;
    size_t total = (size_t)ch->lead + (size_t)(nsym * sym) + 4000;
    double *x = calloc(total, sizeof(double));
    for (int s = 0; s < nsym; s++) {
        int a = ch->lead + (int)lround(s * sym), e = ch->lead + (int)lround((s + 1) * sym);
        int n = e - a, ramp = 80;
        for (int i = 0; i < n; i++) {
            double env = 1.0;
            if (i < ramp) env = 0.5 - 0.5 * cos(PI * i / ramp);
            else if (i >= n - ramp) env = 0.5 - 0.5 * cos(PI * (n - 1 - i) / ramp);
            x[a + i] = ch->amplitude * env * sin(2 * PI * freq[s] * scale * i / WM_SONIC_SAMPLE_RATE);
        }
    }
    free(freq);
    int16_t *pcm = malloc(sizeof(int16_t) * total);
    const int delay = 192;   // 12 ms 回声
    for (size_t i = 0; i < total; i++) {
        double v = x[i];
        if (ch->echo_gain > 0 && i >= (size_t)delay) v += ch->echo_gain * x[i - delay];
        if (ch->burst_at >= 0 && (int)i >= ch->burst_at && (int)i < ch->burst_at + ch->burst_len) v = 0;
        v += ch->noise_rms * gauss();
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)lround(v);
    }
    free(x);
    *out_n = total;
    return pcm;
}

// 按 256 采样一块喂给接收机(与固件任务的读块方式一致),返回收到的帧数。
static int run(wm_sonic_t *rx, const int16_t *pcm, size_t n, const uint8_t *want, int wlen) {
    int frames = 0;
    for (size_t i = 0; i < n; i += 256) {
        size_t chunk = n - i < 256 ? n - i : 256;
        if (wm_sonic_feed(rx, pcm + i, chunk) == WM_SONIC_EV_FRAME) {
            assert(rx->payload_len == wlen);
            assert(memcmp(rx->payload, want, (size_t)wlen) == 0);
            frames++;
        }
    }
    return frames;
}

static wm_sonic_t s_rx;

// 在给定信道下随机发 trials 帧,返回成功帧数。
static int trial(const char *name, channel_t ch, int trials, int min_ok) {
    int ok = 0;
    for (int t = 0; t < trials; t++) {
        uint8_t payload[WM_SONIC_MAX_PAYLOAD];
        int plen = 1 + (int)(rnd() % 60);
        for (int i = 0; i < plen; i++) payload[i] = (uint8_t)rnd();
        channel_t c = ch;
        c.lead = 800 + (int)(rnd() % 1280);   // 任意起始相位(覆盖两个符号内的所有偏移)
        size_t n;
        int16_t *pcm = modulate(payload, plen, &c, &n);
        wm_sonic_init(&s_rx);
        ok += run(&s_rx, pcm, n, payload, plen);
        free(pcm);
    }
    printf("  %-34s %d/%d\n", name, ok, trials);
    assert(ok >= min_ok);
    return ok;
}

int main(void) {
    assert(wm_sonic_crc16((const uint8_t *)"123456789", 9) == 0x29B1);
    assert(wm_sonic_body_parity(3) == 8 && wm_sonic_body_parity(44) == 16 &&
           wm_sonic_body_parity(180) == 60);

    channel_t clean = {.amplitude = 8000, .burst_at = -1};
    trial("clean", clean, 60, 60);

    channel_t loud = {.amplitude = 32000, .burst_at = -1};
    trial("near full scale", loud, 20, 20);

    // 远距离小信号:峰值 120 LSB,底噪 20 LSB。考验定点 Goertzel 的小信号精度。
    channel_t quiet = {.amplitude = 120, .noise_rms = 20, .burst_at = -1};
    trial("quiet (amp 120, noise 20)", quiet, 40, 40);

    // 宽带噪声与信号等功率(全带 SNR 0 dB):嘈杂会场。
    channel_t noisy = {.amplitude = 4000, .noise_rms = 2828, .burst_at = -1};
    trial("SNR 0 dB", noisy, 40, 40);

    // SNR -6 dB:允许个别帧失败,但大多数应靠 RS 纠回来。
    channel_t harsh = {.amplitude = 4000, .noise_rms = 5657, .burst_at = -1};
    trial("SNR -6 dB", harsh, 40, 34);

    channel_t echo = {.amplitude = 6000, .noise_rms = 300, .echo_gain = 0.5, .burst_at = -1};
    trial("echo 12 ms x0.5", echo, 40, 40);

    channel_t fast = {.amplitude = 6000, .noise_rms = 300, .skew_ppm = 300, .burst_at = -1};
    trial("clock skew +300 ppm", fast, 30, 30);
    channel_t slow = {.amplitude = 6000, .noise_rms = 300, .skew_ppm = -300, .burst_at = -1};
    trial("clock skew -300 ppm", slow, 30, 30);

    // 帧体中间被遮挡 120 ms(3 个符号 ≈ 2 字节),RS 必须纠回。
    channel_t burst = {.amplitude = 6000, .noise_rms = 200, .burst_at = 30000, .burst_len = 1920};
    trial("120 ms dropout", burst, 30, 30);

    // 最长帧。
    {
        uint8_t payload[WM_SONIC_MAX_PAYLOAD];
        for (int i = 0; i < WM_SONIC_MAX_PAYLOAD; i++) payload[i] = (uint8_t)rnd();
        channel_t c = {.amplitude = 6000, .noise_rms = 600, .lead = 1234, .burst_at = -1};
        size_t n;
        int16_t *pcm = modulate(payload, WM_SONIC_MAX_PAYLOAD, &c, &n);
        wm_sonic_init(&s_rx);
        assert(run(&s_rx, pcm, n, payload, WM_SONIC_MAX_PAYLOAD) == 1);
        free(pcm);
        puts("  max payload (178 bytes)            1/1");
    }

    // 同一帧连续重播 3 遍(H5 的默认行为):每一遍都应独立解出。
    {
        uint8_t payload[20];
        for (int i = 0; i < 20; i++) payload[i] = (uint8_t)rnd();
        channel_t c = {.amplitude = 6000, .noise_rms = 300, .lead = 9600, .burst_at = -1};
        wm_sonic_init(&s_rx);
        int frames = 0;
        for (int r = 0; r < 3; r++) {
            size_t n;
            int16_t *pcm = modulate(payload, 20, &c, &n);
            frames += run(&s_rx, pcm, n, payload, 20);
            free(pcm);
        }
        assert(frames == 3);
        puts("  3 repeats                          3/3");
    }

    // 纯噪声 5 分钟:不得解出任何帧;误同步(BAD_HEADER)次数应很少,否则会挡住真前导。
    {
        wm_sonic_init(&s_rx);
        int16_t buf[256];
        int frames = 0, false_sync = 0;
        for (int i = 0; i < 5 * 60 * WM_SONIC_SAMPLE_RATE / 256; i++) {
            for (int k = 0; k < 256; k++) buf[k] = (int16_t)lround(3000 * gauss());
            wm_sonic_event_t ev = wm_sonic_feed(&s_rx, buf, 256);
            if (ev == WM_SONIC_EV_FRAME) frames++;
            if (ev == WM_SONIC_EV_BAD_HEADER) false_sync++;
        }
        printf("  5 min white noise: frames=%d false_sync=%d\n", frames, false_sync);
        assert(frames == 0);
        assert(false_sync <= 30);
    }

    // 持续的前导频率单音(例如环境里恰好有 1.6 kHz 的蜂鸣):不得解出帧。
    {
        wm_sonic_init(&s_rx);
        int16_t buf[256];
        int frames = 0;
        for (int i = 0, t = 0; i < 20 * WM_SONIC_SAMPLE_RATE / 256; i++) {
            for (int k = 0; k < 256; k++, t++) {
                buf[k] = (int16_t)lround(5000 * sin(2 * PI * WM_SONIC_F_P0 * t / WM_SONIC_SAMPLE_RATE));
            }
            if (wm_sonic_feed(&s_rx, buf, 256) == WM_SONIC_EV_FRAME) frames++;
        }
        assert(frames == 0);
        puts("  steady 1.6 kHz tone                no frame");
    }

    puts("test_wm_sonic: PASS");
    return 0;
}
