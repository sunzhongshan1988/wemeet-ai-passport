// tests/test_wm_sonic_interop.mjs —— H5 编码器(JS)与固件解码器(C)的互通测试。
// 用 web/sonic/wm-sonic.js 在手机常见采样率下合成声音,重采样到设备的 16 kHz,
// 加噪后交给 C 解码器,断言解出的命令字节与发送的一致。
//   node tests/test_wm_sonic_interop.mjs <wm_sonic_cli 可执行文件>
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { frameSeconds, synthesize } from '../web/sonic/wm-sonic.js';
import { encodeBind, encodeMessage, encodeProvision, encodeReset, encodeServer } from '../web/sonic/wm-proto.js';

const cli = process.argv[2];
if (!cli) {
  console.error('usage: node tests/test_wm_sonic_interop.mjs <wm_sonic_cli>');
  process.exit(2);
}

// 加窗 sinc 重采样到 16 kHz(截止 7 kHz),模拟"手机 DAC → 空气 → 设备 ADC"的采样率转换。
function resampleTo16k(input, fromRate) {
  const ratio = fromRate / 16000;
  const half = 24;
  const cutoff = 7000 / fromRate;
  const out = new Float32Array(Math.floor(input.length / ratio));
  for (let i = 0; i < out.length; i++) {
    const center = i * ratio;
    const first = Math.ceil(center - half);
    let acc = 0;
    for (let k = first; k <= center + half; k++) {
      if (k < 0 || k >= input.length) continue;
      const t = k - center;
      const sinc = t === 0 ? 2 * cutoff : Math.sin(2 * Math.PI * cutoff * t) / (Math.PI * t);
      acc += input[k] * sinc * (0.5 + 0.5 * Math.cos((Math.PI * t) / (half + 1)));
    }
    out[i] = acc;
  }
  return out;
}

let seed = 0x2545f491;
const random = () => {
  seed ^= seed << 13; seed >>>= 0;
  seed ^= seed >>> 17;
  seed ^= seed << 5; seed >>>= 0;
  return seed / 4294967296;
};

const dir = mkdtempSync(join(tmpdir(), 'wm-sonic-'));
const hex = (bytes) => Buffer.from(bytes).toString('hex');
let failures = 0;

function check(name, payload, sampleRate, { amplitude = 6000, noise = 400 } = {}) {
  const audio = resampleTo16k(synthesize(payload, sampleRate), sampleRate);
  const lead = 3000 + Math.floor(random() * 640);
  const pcm = new Int16Array(lead + audio.length + 3200);
  for (let i = 0; i < pcm.length; i++) {
    const signal = i >= lead && i < lead + audio.length ? audio[i - lead] * amplitude : 0;
    const value = signal + noise * (random() + random() + random() + random() - 2) * 1.73;
    pcm[i] = Math.max(-32768, Math.min(32767, Math.round(value)));
  }
  const file = join(dir, 'frame.raw');
  writeFileSync(file, Buffer.from(pcm.buffer));
  let output = '';
  try {
    output = execFileSync(cli, [file], { encoding: 'utf8' });
  } catch (error) {
    output = String(error.stdout ?? '');
  }
  const got = output.trim().split(/\s+/)[0] ?? '';
  const ok = got === hex(payload);
  if (!ok) failures++;
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${name} @${sampleRate} Hz (${payload.length} B, ${frameSeconds(payload.length).toFixed(1)} s)`);
}

const commands = {
  provision: encodeProvision({ ssid: 'WeMeet-会场5G', password: 'Str0ng#Pass w/空格', token: 'K7Q2M9XWTB' }),
  'provision-open': encodeProvision({ ssid: 'Guest' }),
  bind: encodeBind({ token: 'ABCDEFGH23' }),
  server: encodeServer({ url: 'http://192.168.1.20:8787' }),
  reset: encodeReset(),
  message: encodeMessage({ text: '你好,WeMeet Next 👋' }),
};
for (const rate of [48000, 44100, 22050]) {
  for (const [name, payload] of Object.entries(commands)) check(name, payload, rate);
}
check('provision (quiet)', commands.provision, 48000, { amplitude: 200, noise: 30 });
check('provision (SNR ~0 dB)', commands.provision, 44100, { amplitude: 4000, noise: 2800 });

rmSync(dir, { recursive: true, force: true });
if (failures) {
  console.error(`test_wm_sonic_interop: FAIL (${failures})`);
  process.exit(1);
}
console.log('test_wm_sonic_interop: PASS');
