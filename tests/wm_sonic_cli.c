// tests/wm_sonic_cli.c —— 把一个 16 kHz / s16le / 单声道原始 PCM 文件喂给接收机,
// 每解出一帧打印一行十六进制 payload。供 JS 互通测试与手工排查录音使用:
//   cc -Imain tests/wm_sonic_cli.c main/wm_sonic.c main/wm_rs.c -o wm_sonic_cli
//   ./wm_sonic_cli capture.raw
#include "wm_sonic.h"

#include <stdio.h>

static wm_sonic_t s_rx;

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <pcm_s16le_16k_mono.raw>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    wm_sonic_init(&s_rx);
    int16_t buf[256];
    size_t n;
    int frames = 0;
    while ((n = fread(buf, sizeof(buf[0]), 256, f)) > 0) {
        wm_sonic_event_t ev = wm_sonic_feed(&s_rx, buf, n);
        if (ev == WM_SONIC_EV_FRAME) {
            for (int i = 0; i < s_rx.payload_len; i++) printf("%02x", s_rx.payload[i]);
            printf(" corrected=%d\n", s_rx.corrected);
            frames++;
        } else if (ev == WM_SONIC_EV_BAD_BODY) {
            fprintf(stderr, "bad body\n");
        }
    }
    fclose(f);
    return frames > 0 ? 0 : 1;
}
