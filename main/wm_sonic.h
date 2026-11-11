// main/wm_sonic.h —— WeMeet Sonic v1 声波数据链路的接收端(纯逻辑,可 host 测试)。
//
// 手机 H5 用扬声器播放一段可闻频段的多音调制信号,设备用麦克风收下来解出一帧字节。
// 设计取舍(都围绕"手机小喇叭 + 嘈杂会场 + 无 FPU 的 ESP32-C3"):
//   - 16-FSK:每个符号只发 1 个音,16 个候选频率 = 4 bit。单音包络恒定,手机喇叭可以
//     开到最大而不削波;接收端只需比较 16 个频点的能量,不需要相位/均衡。
//   - 频段 1.6–5.0 kHz:手机喇叭在这一段响度最高,也远离 16 kHz 采样的奈奎斯特频率。
//   - 符号 40 ms,只分析中间 30 ms,两侧各 5 ms 保护间隔吞掉房间混响与符号切换瞬态。
//   - 频点间隔 200 Hz = 30 ms 窗下的 6 个 bin,彼此严格正交。
//   - 全整数 Goertzel:C3 没有浮点单元,软浮点在音频路径上太慢。
//
// 帧结构(与 web/sonic/wm-sonic.js 必须一致):
//   [前导 7 符号: Barker-7,用 P0/P1 两个专用频率]
//   [帧头 12 符号 = 6 字节: len, 0xA1, RS 校验 4 字节]      len = 帧体数据长度
//   [帧体 2*(len+npar) 符号: payload, CRC16(2), RS 校验 npar 字节]
//   其中 npar = wm_sonic_body_parity(len);payload 长度 = len - 2。
// 符号内高半字节在前。速率约 12.5 字节/秒;一条 Wi-Fi 配网命令约 5 秒。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define WM_SONIC_SAMPLE_RATE   16000   // 设备侧采样率(Hz);BSP 的 I2S 固定为 16 kHz
#define WM_SONIC_SYMBOL        640     // 每符号采样数 = 40 ms
#define WM_SONIC_WINDOW        480     // 每符号分析窗 = 30 ms(居中)
#define WM_SONIC_GUARD         80      // 分析窗两侧保护间隔 = 5 ms
#define WM_SONIC_HOP           80      // 搜索前导时的滑动步长 = 5 ms
#define WM_SONIC_TONES         16      // 数据频点个数(4 bit/符号)
#define WM_SONIC_F_DATA0       2000    // 数据频点 0 的频率(Hz)
#define WM_SONIC_F_STEP        200     // 相邻数据频点间隔(Hz)
#define WM_SONIC_F_P0          1600    // 前导"正"频率(Hz)
#define WM_SONIC_F_P1          1800    // 前导"负"频率(Hz)
#define WM_SONIC_PREAMBLE_LEN  7       // Barker-7: + + + - - + -
#define WM_SONIC_HEADER_MAGIC  0xA1    // 帧头第 2 字节:高 4 位协议族 0xA,低 4 位版本 1
#define WM_SONIC_HEADER_PARITY 4       // 帧头 RS 校验字节数(可纠 2 字节)
#define WM_SONIC_MAX_BODY      180     // 帧体数据最大长度(payload + CRC16)
#define WM_SONIC_MAX_PAYLOAD   (WM_SONIC_MAX_BODY - 2)

// 采样环形缓冲长度,必须是 2 的幂且 > 确认期(640) + 一个符号(640) + 分析窗(480)。
#define WM_SONIC_RING          2048

// 接收状态,UI 用它决定显示"聆听中 / 接收中"。
typedef enum {
    WM_SONIC_SEARCH = 0,   // 滑动搜索前导
    WM_SONIC_CONFIRM,      // 相关值已过门限,再看一个符号时长以找到峰值中心
    WM_SONIC_HEADER,       // 正在收帧头
    WM_SONIC_BODY,         // 正在收帧体
} wm_sonic_state_t;

// wm_sonic_feed() 的返回事件。一次 feed 内若发生多个事件,返回优先级最高的一个
// (FRAME > BAD_* > SYNC > NONE);一次 feed 的数据量远小于一帧,所以不会丢帧。
typedef enum {
    WM_SONIC_EV_NONE = 0,
    WM_SONIC_EV_SYNC,        // 刚锁定一个前导,开始收帧
    WM_SONIC_EV_BAD_HEADER,  // 帧头纠错失败或字段非法(多半是误同步)
    WM_SONIC_EV_BAD_BODY,    // 帧体纠错失败或 CRC 不符,需要发送端重播
    WM_SONIC_EV_FRAME,       // 收到完整一帧:payload / payload_len 有效
} wm_sonic_event_t;

// 接收机实例。约 4.8 KB,请放在静态区或堆上,不要放在任务栈里。
// 单任务使用:同一实例不得被两个任务同时 feed;UI 任务只读 state/progress/level 这类
// 单字段快照是允许的(读到旧值无害)。
typedef struct {
    int16_t  ring[WM_SONIC_RING];     // 最近的 PCM 采样,按 count 取模写入
    uint32_t count;                   // 已喂入的采样总数(绝对时间轴,回绕无害)
    wm_sonic_state_t state;
    uint32_t hop_fill;                // SEARCH/CONFIRM:距下一次滑动分析还差多少采样
    int16_t  d_hist[64];              // 最近 64 步的前导判别量 d(Q10,-1024..1024)
    uint32_t d_pos;                   // d_hist 的写指针(步数)
    int32_t  confirm_c[9];            // CONFIRM:过门限后连续 9 步的相关值
    int      confirm_n;               // confirm_c 已填个数
    uint32_t confirm_t0;              // 第一次过门限时的 count
    uint32_t next_end;                // HEADER/BODY:下一个符号分析窗的结束时刻(count)
    int      sym_index;               // 当前块内已收符号数
    int      sym_total;               // 当前块应收符号数
    uint8_t  block[255];              // 当前块的字节(帧头或帧体 + 校验)
    uint8_t  body_len;                // 帧头给出的帧体数据长度
    uint8_t  payload[WM_SONIC_MAX_PAYLOAD];  // 最近一帧的 payload(EV_FRAME 后有效)
    uint8_t  payload_len;
    uint8_t  corrected;               // 最近一帧 RS 纠正的字节数(链路质量指标)
    uint8_t  progress;                // 0..100:当前帧接收进度(UI 进度条)
    uint16_t level;                   // 最近分析窗的 RMS 幅度(0..32767,UI 电平表)
} wm_sonic_t;

// 复位接收机到搜索状态。任何时候都可调用(例如重新进入"接收命令"页面时)。
void wm_sonic_init(wm_sonic_t *rx);

// 喂入 16 kHz / 16 bit / 单声道 PCM。不阻塞、无堆分配。
// CPU:搜索态每 5 ms 做 2 个频点 × 480 点;接收态每 40 ms 做 16 个频点 × 480 点。
wm_sonic_event_t wm_sonic_feed(wm_sonic_t *rx, const int16_t *pcm, size_t samples);

// 帧体 RS 校验字节数:每 6 个数据字节配 2 个校验字节,最少 8、最多 60。
// 编码端(JS)与解码端共用这一条规则。
int wm_sonic_body_parity(int body_len);

// CRC-16/CCITT-FALSE(poly 0x1021,init 0xFFFF,不反转)。帧体内高字节在前。
uint16_t wm_sonic_crc16(const uint8_t *data, size_t len);
