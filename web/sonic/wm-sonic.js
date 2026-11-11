// web/sonic/wm-sonic.js —— WeMeet Sonic v1 发送端(H5 / 浏览器 / Node 通用,零依赖 ES module)。
//
// 把一段字节调制成手机扬声器可以播放的声音,设备固件(main/wm_sonic.c)用麦克风解出来。
// 这里的所有常量都必须与 main/wm_sonic.h 保持一致;tests/test_wm_sonic_interop.mjs
// 会用本文件生成 PCM 喂给 C 解码器,任何一边改了参数测试就会失败。
//
// 用法(H5):
//   import { playFrame } from './wm-sonic.js';
//   import { encodeProvision } from './wm-proto.js';
//   const handle = await playFrame(encodeProvision({ ssid, password, token }), { repeat: 3 });
//   handle.stop();           // 用户取消,或服务器已确认设备上线时调用
//
// 注意:浏览器要求由用户手势(点击)触发后才能出声;iOS 静音拨片会把 WebAudio 一起静音,
// 页面上应提示用户打开铃声并把媒体音量调到最大。

export const WM_SONIC = Object.freeze({
  symbolSeconds: 0.04,        // 每符号 40 ms
  rampSeconds: 0.005,         // 符号两端 5 ms 升余弦淡入淡出(落在接收端的保护间隔里)
  toneCount: 16,
  dataBaseHz: 2000,
  dataStepHz: 200,
  preambleHz: [1600, 1800],   // [P0, P1]
  barker: [1, 1, 1, -1, -1, 1, -1],
  wakeHz: 1200,               // 唤醒音:不参与解码,只为让手机功放/自动增益先稳定下来
  wakeSeconds: 0.12,
  headerMagic: 0xa1,
  headerParity: 4,
  maxBody: 180,
  maxPayload: 178,
});

// ---- GF(2^8) Reed-Solomon 编码(本原多项式 0x11D,fcr = 0)----
const GF_EXP = new Uint8Array(512);
const GF_LOG = new Uint8Array(256);
(() => {
  let x = 1;
  for (let i = 0; i < 255; i++) {
    GF_EXP[i] = x;
    GF_LOG[x] = i;
    x <<= 1;
    if (x & 0x100) x ^= 0x11d;
  }
  for (let i = 255; i < 512; i++) GF_EXP[i] = GF_EXP[i - 255];
})();

function gfMul(a, b) {
  return a === 0 || b === 0 ? 0 : GF_EXP[GF_LOG[a] + GF_LOG[b]];
}

/** 系统码编码,返回 npar 个校验字节。 */
export function rsEncode(data, npar) {
  const gen = new Uint8Array(npar + 1);
  gen[0] = 1;
  for (let i = 0; i < npar; i++) {
    for (let j = i + 1; j > 0; j--) gen[j] ^= gfMul(gen[j - 1], GF_EXP[i]);
  }
  const parity = new Uint8Array(npar);
  for (const byte of data) {
    const fb = byte ^ parity[0];
    parity.copyWithin(0, 1);
    parity[npar - 1] = 0;
    if (fb) for (let j = 0; j < npar; j++) parity[j] ^= gfMul(fb, gen[j + 1]);
  }
  return parity;
}

/** CRC-16/CCITT-FALSE。 */
export function crc16(data) {
  let crc = 0xffff;
  for (const byte of data) {
    crc ^= byte << 8;
    for (let b = 0; b < 8; b++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

/** 帧体校验字节数:每 6 字节配 2 字节,最少 8、最多 60。与固件 wm_sonic_body_parity 一致。 */
export function bodyParity(bodyLen) {
  return Math.min(60, Math.max(8, 2 * Math.ceil(bodyLen / 6)));
}

/**
 * 把 payload 打成符号序列。
 * @param {Uint8Array} payload 1..178 字节
 * @returns {{ preamble: number[], data: number[] }} preamble 为 ±1,data 为 0..15
 */
export function buildSymbols(payload) {
  if (!(payload instanceof Uint8Array)) throw new TypeError('payload must be a Uint8Array');
  if (payload.length < 1 || payload.length > WM_SONIC.maxPayload) {
    throw new RangeError(`payload length ${payload.length} out of range 1..${WM_SONIC.maxPayload}`);
  }
  const body = new Uint8Array(payload.length + 2);
  body.set(payload);
  const crc = crc16(payload);
  body[payload.length] = crc >> 8;
  body[payload.length + 1] = crc & 0xff;

  const header = Uint8Array.of(body.length, WM_SONIC.headerMagic);
  const bytes = [
    ...header,
    ...rsEncode(header, WM_SONIC.headerParity),
    ...body,
    ...rsEncode(body, bodyParity(body.length)),
  ];
  const data = [];
  for (const byte of bytes) data.push(byte >> 4, byte & 0x0f);
  return { preamble: [...WM_SONIC.barker], data };
}

/** 一帧的播放时长(秒),用于 UI 进度条。 */
export function frameSeconds(payloadLength) {
  const body = payloadLength + 2;
  const symbols = WM_SONIC.barker.length + 2 * (2 + WM_SONIC.headerParity) + 2 * (body + bodyParity(body));
  return WM_SONIC.wakeSeconds + symbols * WM_SONIC.symbolSeconds + 0.04;
}

/**
 * 合成一帧 PCM(单声道 Float32,-1..1)。
 * @param {Uint8Array} payload
 * @param {number} sampleRate 输出采样率(AudioContext.sampleRate,通常 44100 或 48000)
 * @param {{ gain?: number }} [options] gain 默认 0.9
 */
export function synthesize(payload, sampleRate, options = {}) {
  const gain = options.gain ?? 0.9;
  const { preamble, data } = buildSymbols(payload);
  const tones = [
    ...preamble.map((sign) => WM_SONIC.preambleHz[sign > 0 ? 0 : 1]),
    ...data.map((nibble) => WM_SONIC.dataBaseHz + nibble * WM_SONIC.dataStepHz),
  ];
  const wake = Math.round(WM_SONIC.wakeSeconds * sampleRate);
  const tail = Math.round(0.04 * sampleRate);
  // 符号边界按"绝对时间 × 采样率"取整,避免 44.1 kHz 下每符号 1764 采样的舍入误差累积。
  const boundary = (i) => wake + Math.round(i * WM_SONIC.symbolSeconds * sampleRate);
  const out = new Float32Array(boundary(tones.length) + tail);
  const ramp = Math.max(1, Math.round(WM_SONIC.rampSeconds * sampleRate));

  const writeTone = (start, end, hz) => {
    const n = end - start;
    const step = (2 * Math.PI * hz) / sampleRate;
    for (let i = 0; i < n; i++) {
      let envelope = 1;
      if (i < ramp) envelope = 0.5 - 0.5 * Math.cos((Math.PI * i) / ramp);
      else if (i >= n - ramp) envelope = 0.5 - 0.5 * Math.cos((Math.PI * (n - 1 - i)) / ramp);
      out[start + i] = gain * envelope * Math.sin(step * i);
    }
  };

  writeTone(0, wake, WM_SONIC.wakeHz);
  tones.forEach((hz, i) => writeTone(boundary(i), boundary(i + 1), hz));
  return out;
}

/**
 * 在浏览器里播放一帧(可重复)。必须在用户手势回调里调用。
 * @param {Uint8Array} payload
 * @param {{ repeat?: number, gapSeconds?: number, gain?: number, audioContext?: AudioContext,
 *           onProgress?: (fraction: number) => void }} [options]
 * @returns {Promise<{ stop: () => void, done: Promise<void>, seconds: number }>}
 */
export async function playFrame(payload, options = {}) {
  const repeat = options.repeat ?? 3;
  const gap = options.gapSeconds ?? 0.6;
  const Ctx = globalThis.AudioContext ?? globalThis.webkitAudioContext;
  if (!Ctx) throw new Error('当前浏览器不支持 WebAudio,无法发送声波');
  const ctx = options.audioContext ?? new Ctx();
  if (ctx.state === 'suspended') await ctx.resume();

  const pcm = synthesize(payload, ctx.sampleRate, { gain: options.gain });
  const gapSamples = Math.round(gap * ctx.sampleRate);
  const buffer = ctx.createBuffer(1, repeat * (pcm.length + gapSamples), ctx.sampleRate);
  const channel = buffer.getChannelData(0);
  for (let r = 0; r < repeat; r++) channel.set(pcm, r * (pcm.length + gapSamples));

  const source = ctx.createBufferSource();
  source.buffer = buffer;
  source.connect(ctx.destination);
  const seconds = buffer.duration;
  const startedAt = ctx.currentTime;
  let timer = null;
  const done = new Promise((resolve) => {
    source.onended = () => {
      if (timer) clearInterval(timer);
      options.onProgress?.(1);
      if (!options.audioContext) ctx.close().catch(() => {});
      resolve();
    };
  });
  if (options.onProgress) {
    timer = setInterval(() => options.onProgress(Math.min(1, (ctx.currentTime - startedAt) / seconds)), 100);
  }
  source.start();
  return {
    seconds,
    done,
    stop: () => {
      try {
        source.stop();
      } catch {
        /* 已经结束 */
      }
    },
  };
}
