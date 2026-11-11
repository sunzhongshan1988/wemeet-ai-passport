// web/sonic/wm-proto.js —— WeMeet 设备命令帧编码(声波链路的 payload)。
// 与固件 main/wm_proto.h 一一对应:payload = [cmd][seq][TLV...],TLV = [tag][len][value]。
// 设备会跳过不认识的 tag,所以以后加字段不会破坏旧固件。

export const WM_CMD = Object.freeze({
  PROVISION: 0x01, // 配网(可同时携带绑定令牌,一步完成"配网 + 绑定")
  BIND: 0x02,      // 仅绑定:设备已联网,只下发绑定令牌
  SERVER: 0x03,    // 设置服务器地址(联调 / 私有化部署)
  RESET: 0x04,     // 清除配网与绑定(设备端仍需按键确认)
  MESSAGE: 0x05,   // 在设备屏幕上显示一条文本(联调 / 找设备)
});

export const WM_TAG = Object.freeze({
  SSID: 0x01,       // UTF-8,1..32 字节
  PASSWORD: 0x02,   // UTF-8,0..64 字节;开放网络可省略
  BIND_TOKEN: 0x03, // ASCII,1..24 字节;服务器签发的一次性绑定令牌
  URL: 0x04,        // ASCII,1..96 字节;形如 https://api.example.com
  TEXT: 0x05,       // UTF-8,1..96 字节
});

const LIMITS = {
  [WM_TAG.SSID]: [1, 32],
  [WM_TAG.PASSWORD]: [0, 64],
  [WM_TAG.BIND_TOKEN]: [1, 24],
  [WM_TAG.URL]: [1, 96],
  [WM_TAG.TEXT]: [1, 96],
};

const utf8 = new TextEncoder();

function build(cmd, fields) {
  const parts = [cmd, Math.floor(Math.random() * 256)];
  for (const [tag, value] of fields) {
    if (value === undefined || value === null) continue;
    const bytes = utf8.encode(String(value));
    const [min, max] = LIMITS[tag];
    if (bytes.length < min || bytes.length > max) {
      throw new RangeError(`字段 0x${tag.toString(16)} 长度 ${bytes.length} 超出 ${min}..${max} 字节`);
    }
    parts.push(tag, bytes.length, ...bytes);
  }
  if (parts.length > 178) throw new RangeError('命令过长,超出单帧 178 字节');
  return Uint8Array.from(parts);
}

/** 配网;带 token 时设备联网后会自动向服务器认领,完成绑定。 */
export function encodeProvision({ ssid, password = '', token }) {
  return build(WM_CMD.PROVISION, [
    [WM_TAG.SSID, ssid],
    [WM_TAG.PASSWORD, password],
    [WM_TAG.BIND_TOKEN, token],
  ]);
}

/** 仅绑定(设备已联网)。 */
export function encodeBind({ token }) {
  return build(WM_CMD.BIND, [[WM_TAG.BIND_TOKEN, token]]);
}

/** 设置设备访问的服务器地址。 */
export function encodeServer({ url }) {
  return build(WM_CMD.SERVER, [[WM_TAG.URL, url]]);
}

/** 请求设备清除配网与绑定。 */
export function encodeReset() {
  return build(WM_CMD.RESET, []);
}

/** 在设备上显示一条文本。 */
export function encodeMessage({ text }) {
  return build(WM_CMD.MESSAGE, [[WM_TAG.TEXT, text]]);
}
