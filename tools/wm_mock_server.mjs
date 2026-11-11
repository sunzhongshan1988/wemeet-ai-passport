#!/usr/bin/env node
// tools/wm_mock_server.mjs -- reference implementation of the WeMeet device API.
//
// Zero-dependency Node server for firmware development and for the WeMeet Next backend /
// front-end teams to read: it implements every endpoint in docs/wemeet/README.md with
// in-memory state, and serves the two reference pages in web/:
//
//   /h5   phone page  -- enter Wi-Fi, create a bind session, send it to the device by sound
//   /pc   desktop page -- device list, pairing-code entry, remote commands, unbind
//
// Usage:  node tools/wm_mock_server.mjs [port]        (default 8787)
// Then point a device at it with the sonic SERVER command (button on /h5), using the
// LAN address this script prints. State is lost on restart; nothing here is production
// security (single implicit user, plain HTTP).

import { createServer } from 'node:http';
import { randomBytes, randomInt } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { networkInterfaces } from 'node:os';
import { dirname, extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const PORT = Number(process.argv[2] ?? 8787);
const WEB_ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', 'web');
const USER = { id: 'u_demo', name: '演示用户' };
const BIND_TTL = 600;
const ONLINE_WINDOW_MS = 90_000;

/** deviceId -> device record */
const devices = new Map();
/** bind session id -> { token, expiresAt, deviceId } */
const sessions = new Map();
/** pairing code -> { deviceId, expiresAt } */
const pairings = new Map();
let nextCommandId = 1;
let nextNoticeId = 1;

const now = () => Date.now();
const base32 = (bytes) => [...bytes].map((b) => 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'[b % 32]).join('');

function demoContent(device) {
  return {
    card: {
      saved: true, name: USER.name, title: '产品负责人', org: 'WeMeet', avatarRevision: '',
      contactCode: `BEGIN:VCARD\r\nVERSION:3.0\r\nFN:${USER.name}\r\nORG:WeMeet\r\nEND:VCARD\r\n`,
    },
    badge: {
      event: 'WeMeet Next 开发者大会',
      name: USER.name,
      title: '产品负责人',
      org: 'WeMeet',
      checkinCode: `https://wemeet.example/c/${device.id}`,
      checkedIn: device.checkedIn,
    },
    agenda: [
      { time: '09:30', title: '签到入场', place: '一层大厅' },
      { time: '10:00', title: '主题演讲:智能体如何办活动', place: '主会场' },
      { time: '14:00', title: '分论坛:硬件与现场体验', place: '3 号厅' },
    ],
  };
}

function upsertDevice(body) {
  let device = devices.get(body.deviceId);
  if (!device) {
    device = {
      id: body.deviceId, secret: body.secret, model: body.model, fw: body.fw,
      ownerId: null, token: null, name: '', rev: 1, checkedIn: false,
      notices: [], commands: [], lastSeen: 0, battery: null, rssi: null, boundAt: null,
    };
    devices.set(device.id, device);
  }
  return device;
}

function bind(device) {
  device.ownerId = USER.id;
  device.token = randomBytes(24).toString('base64url');   // revokes any earlier token
  device.boundAt = now();
  device.rev += 1;
  device.name ||= `Passport ${device.id.slice(-4)}`;
  return { deviceToken: device.token, owner: { name: USER.name } };
}

function unbind(device) {
  device.ownerId = null;
  device.token = null;
  device.commands = [];
}

function publicDevice(device) {
  return {
    id: device.id, name: device.name, model: device.model, firmware: device.fw,
    online: now() - device.lastSeen < ONLINE_WINDOW_MS,
    lastSeenAt: device.lastSeen || null, battery: device.battery, rssi: device.rssi,
    boundAt: device.boundAt, pendingCommands: device.commands.length,
  };
}

const fail = (status, code, message) => ({ status, body: { error: { code, message } } });
const ok = (body) => ({ status: 200, body });

// ---- device-facing API: /device/v1 ----
const deviceRoutes = {
  'POST /device/v1/claim': (body) => {
    const session = [...sessions.values()].find((s) => s.token === body.token);
    if (!session) return fail(404, 'token_invalid', '绑定码无效,请在 WeMeet 重新发起');
    if (session.expiresAt < now()) return fail(410, 'token_expired', '绑定码已过期,请在 WeMeet 重新发起');
    if (session.deviceId) return fail(409, 'token_used', '绑定码已被使用');
    const device = upsertDevice(body);
    // Trust on first use: a known device must present the secret it registered with.
    if (device.secret !== body.secret) return fail(403, 'secret_mismatch', '设备校验失败');
    session.deviceId = device.id;
    return ok(bind(device));
  },
  'POST /device/v1/pairing': (body) => {
    const device = upsertDevice(body);
    if (device.secret !== body.secret) return fail(403, 'secret_mismatch', '设备校验失败');
    for (const [code, entry] of pairings) if (entry.deviceId === device.id) pairings.delete(code);
    let code;
    do code = String(randomInt(0, 1_000_000)).padStart(6, '0'); while (pairings.has(code));
    pairings.set(code, { deviceId: device.id, expiresAt: now() + BIND_TTL * 1000 });
    return ok({ code, url: `http://${lanAddress()}:${PORT}/pc?c=${code}`, expiresIn: BIND_TTL, interval: 3 });
  },
  'POST /device/v1/pairing/poll': (body) => {
    const device = devices.get(body.deviceId);
    if (!device || device.secret !== body.secret) return fail(403, 'secret_mismatch', '设备校验失败');
    if (device.ownerId && device.pairedPending) {
      device.pairedPending = false;
      return ok({ status: 'bound', deviceToken: device.token, owner: { name: USER.name } });
    }
    const entry = [...pairings.values()].find((p) => p.deviceId === device.id);
    if (!entry || entry.expiresAt < now()) return ok({ status: 'expired' });
    return ok({ status: 'pending', expiresIn: Math.round((entry.expiresAt - now()) / 1000) });
  },
  'GET /device/v1/sync': (_body, query, device) => {
    device.lastSeen = now();
    device.battery = Number(query.get('bat'));
    device.rssi = Number(query.get('rssi'));
    device.fw = query.get('fw') ?? device.fw;
    const ack = Number(query.get('ack') ?? 0);
    device.commands = device.commands.filter((c) => c.id !== ack);
    const common = {
      rev: device.rev, serverTime: Math.floor(now() / 1000), tzOffsetMin: 480, nextPollSec: 10,
      command: device.commands[0] ?? null,
    };
    if (Number(query.get('rev')) === device.rev) return ok({ ...common, unchanged: true });
    return ok({ ...common, owner: { name: USER.name }, ...demoContent(device), notices: device.notices.slice(0, 4) });
  },
  'POST /device/v1/unbind': (_body, _query, device) => {
    unbind(device);
    return ok({});
  },
};

// ---- user-facing API (what H5 / PC call; real backend adds login + workspace scoping) ----
const userRoutes = {
  'POST /api/devices/bind-sessions': () => {
    const id = randomBytes(8).toString('hex');
    const token = base32(randomBytes(10));
    sessions.set(id, { token, expiresAt: now() + BIND_TTL * 1000, deviceId: null });
    return ok({ id, token, expiresIn: BIND_TTL });
  },
  'POST /api/devices/claim-by-code': (body) => {
    const entry = pairings.get(String(body.code ?? '').trim());
    if (!entry || entry.expiresAt < now()) return fail(404, 'code_invalid', '配对码无效或已过期');
    const device = devices.get(entry.deviceId);
    pairings.delete(String(body.code).trim());
    bind(device);
    device.pairedPending = true;   // delivered to the device on its next pairing poll
    return ok({ device: publicDevice(device) });
  },
  'GET /api/devices': () =>
    ok({ devices: [...devices.values()].filter((d) => d.ownerId === USER.id).map(publicDevice) }),
};

function dynamicUserRoute(method, path, body) {
  let match = path.match(/^\/api\/devices\/bind-sessions\/([0-9a-f]+)$/);
  if (match && method === 'GET') {
    const session = sessions.get(match[1]);
    if (!session) return fail(404, 'not_found', 'bind session not found');
    if (session.deviceId) return ok({ status: 'claimed', device: publicDevice(devices.get(session.deviceId)) });
    return ok({ status: session.expiresAt < now() ? 'expired' : 'pending' });
  }
  match = path.match(/^\/api\/devices\/(WM-[0-9A-F]{12})(\/commands|\/notices)?$/);
  if (!match) return null;
  const device = devices.get(match[1]);
  if (!device || device.ownerId !== USER.id) return fail(404, 'not_found', 'device not found');
  if (!match[2] && method === 'DELETE') {
    unbind(device);   // the device learns about it from the 401 on its next sync
    return ok({});
  }
  if (!match[2] && method === 'PATCH') {
    if (typeof body.name === 'string') device.name = body.name.slice(0, 40);
    if (typeof body.checkedIn === 'boolean') { device.checkedIn = body.checkedIn; device.rev += 1; }
    return ok({ device: publicDevice(device) });
  }
  if (match[2] === '/commands' && method === 'POST') {
    const allowed = ['locate', 'message', 'reboot', 'brightness'];
    if (!allowed.includes(body.type)) return fail(400, 'bad_command', `type must be one of ${allowed}`);
    const command = { id: nextCommandId++, type: body.type, text: String(body.text ?? '').slice(0, 96), value: Number(body.value ?? 0) };
    device.commands.push(command);
    return ok({ command });
  }
  if (match[2] === '/notices' && method === 'POST') {
    device.notices.unshift({ id: nextNoticeId++, title: String(body.title ?? '').slice(0, 40), body: String(body.body ?? '').slice(0, 150) });
    device.rev += 1;
    return ok({});
  }
  return null;
}

function lanAddress() {
  for (const list of Object.values(networkInterfaces())) {
    for (const item of list ?? []) if (item.family === 'IPv4' && !item.internal) return item.address;
  }
  return 'localhost';
}

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8' };

async function serveStatic(path, response) {
  const file = path === '/h5' ? 'h5.html' : path === '/pc' ? 'pc.html' : path.replace(/^\/+/, '');
  const full = normalize(join(WEB_ROOT, file));
  if (!full.startsWith(normalize(WEB_ROOT))) return false;
  try {
    const data = await readFile(full);
    response.writeHead(200, { 'Content-Type': MIME[extname(full)] ?? 'application/octet-stream' });
    response.end(data);
    return true;
  } catch {
    return false;
  }
}

createServer(async (request, response) => {
  const url = new URL(request.url, 'http://localhost');
  const key = `${request.method} ${url.pathname}`;
  let raw = '';
  for await (const chunk of request) raw += chunk;
  let body = {};
  try {
    if (raw) body = JSON.parse(raw);
  } catch {
    /* treated as empty body */
  }

  let result = null;
  if (deviceRoutes[key]) {
    if (key === 'GET /device/v1/sync' || key === 'POST /device/v1/unbind') {
      const token = (request.headers.authorization ?? '').replace(/^Bearer /, '');
      const device = [...devices.values()].find((d) => d.token && d.token === token);
      result = device ? deviceRoutes[key](body, url.searchParams, device) : fail(401, 'unauthorized', '设备未绑定');
    } else {
      result = body.deviceId && body.secret ? deviceRoutes[key](body) : fail(400, 'bad_request', 'deviceId and secret are required');
    }
  } else if (userRoutes[key]) {
    result = userRoutes[key](body);
  } else {
    result = dynamicUserRoute(request.method, url.pathname, body);
  }

  if (result) {
    console.log(`${new Date().toISOString().slice(11, 19)} ${key} -> ${result.status}`);
    response.writeHead(result.status, { 'Content-Type': 'application/json; charset=utf-8' });
    response.end(JSON.stringify(result.body));
  } else if (request.method !== 'GET' || !(await serveStatic(url.pathname, response))) {
    response.writeHead(404, { 'Content-Type': 'application/json' });
    response.end(JSON.stringify({ error: { code: 'not_found', message: 'not found' } }));
  }
}).listen(PORT, () => {
  const host = lanAddress();
  console.log(`WeMeet device mock server`);
  console.log(`  phone page : http://${host}:${PORT}/h5`);
  console.log(`  desktop    : http://${host}:${PORT}/pc`);
  console.log(`  device API : http://${host}:${PORT}   (send this to the device with the SERVER command)`);
});
