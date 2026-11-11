<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# WeMeet Passport: Device, Sonic Protocol, and API Contract

WeMeet Passport is the dedicated hardware for WeMeet Next: an electronic attendee badge
worn on a lanyard. This document is the single contract between firmware, the mobile H5
page, the desktop page, and the backend. The firmware lives in [`main/`](../../main); a
runnable reference implementation lives in [`web/`](../../web) and
[`tools/wm_mock_server.mjs`](../../tools/wm_mock_server.mjs).

## 1. What the Device Does

| Capability | Description |
| --- | --- |
| My business card | First page: portrait, name, role and company; OK displays the saved card’s vCard contact QR; long OK returns |
| Agenda | Up to 5 entries: time, title, place |
| Notices | Keeps 4, previews 3; OK enters the list, UP/DOWN selects any entry, OK opens detail, long OK returns one level. Detail supports scrolling; new notices wake the screen and alert |
| Device page | Device ID, bound account, Wi-Fi, firmware version; device menu (receive sonic command / unbind / factory reset) |
| Sonic commands | The microphone receives commands played by the phone: provision Wi-Fi, bind, set server URL, factory reset, show text |
| Remote management | From the desktop page: locate, show a message, set brightness, reboot, unbind |

The status bar always shows WeMeet at the top left, the time in the center ("--:--" before clock sync), and signal/battery at the right.

Normal H5/PC setup sends Wi-Fi and the binding token directly; no preparation/server-address command is required. Server-address transmission is an independent debug-only action. The built-in production default is used when no device-specific server override is saved.

Keys (UP / DOWN / OK):

| Context | UP / DOWN | OK click | OK long press |
| --- | --- | --- | --- |
| Home pages | Switch page | Card: contact QR; notices: open list; device: open menu | Enter "receive sonic command" |
| Welcome page | - | Enter "receive sonic command" | Same as click |
| Pairing-code page | - | Device menu | Enter "receive sonic command" |
| Notice list | Select entry | Open detail | Return to notices overview |
| Notice detail | Scroll content | - | Return to list, preserving selection |
| Contact QR | - | - | Return to card |
| Menu | Move | Confirm | Back |
| Listening / connecting | - | - | Cancel |
| Message page | Continue | Continue | Continue |

The backlight dims after 20 seconds without input and turns off after 60 seconds. While
the screen is off, the first key press only wakes it.

## 2. Sonic Link: WeMeet Sonic v1

The H5 page plays through the phone speaker and the device microphone receives. The link
is one-way. Transmitter: [`web/sonic/wm-sonic.js`](../../web/sonic/wm-sonic.js). Receiver:
[`main/wm_sonic.c`](../../main/wm_sonic.c).

| Parameter | Value | Reason |
| --- | --- | --- |
| Modulation | 16-FSK, one tone per symbol = 4 bits | A single tone has a constant envelope, so the phone speaker can run at full volume without clipping |
| Data tones | 2000 + 200 x k Hz, k = 0..15 | The loudest band of phone speakers; strictly orthogonal over a 30 ms window |
| Preamble tones | P0 = 1600 Hz, P1 = 1800 Hz | Separate from the data tones so data is never mistaken for a preamble |
| Symbol length | 40 ms with 5 ms raised-cosine ramps at both ends | The receiver analyses only the middle 30 ms, skipping reverberation and transitions |
| Preamble | 7-symbol Barker code `+ + + - - + -` (+ = P0) | Low autocorrelation side lobes; timing within 5 ms |
| Wake tone | 120 ms of 1200 Hz before the frame | Lets the phone amplifier settle; not decoded |
| Receiver sampling | 16 kHz / 16-bit / mono | The BSP fixes I2S at 16 kHz |

Frame layout (high nibble first within a byte):

```text
[preamble: 7 symbols]
[header: 6 bytes = len, 0xA1, 4 RS parity bytes]       len = payload length + 2
[body: payload, 2-byte CRC16, npar RS parity bytes]
npar = clamp(2 x ceil(len / 6), 8, 60)
```

- Error correction: Reed-Solomon over GF(2^8), primitive polynomial `0x11D`, generator
  roots alpha^0..alpha^(npar-1). The header corrects 2 bytes; the body corrects npar / 2
  bytes (about 1 byte in every 6).
- Integrity: CRC-16/CCITT-FALSE (polynomial `0x1021`, initial value `0xFFFF`), high byte first.
- The payload is at most 178 bytes. Throughput is about 12.5 bytes per second: a typical
  provisioning command takes 5 to 7 seconds.
- The H5 page repeats a frame 3 times with 0.6 s gaps; the device drops repeats by
  `(cmd, seq, length)`.

Channel conditions covered by host tests (see [`tests/test_wm_sonic.c`](../../tests/test_wm_sonic.c)):
full-band SNR of 0 dB and -6 dB, a weak signal of 120 LSB peak, a 12 ms echo, clock skew
of +/-300 ppm, a 120 ms dropout, and 5 minutes of noise without a false frame. These are
simulations; behavior with real phones and venues still needs on-device verification.

**Security boundary.** Sound is a cleartext broadcast: anyone present can record it and
decode the Wi-Fi password. The v1 mitigations are: the microphone is on only after the
user presses a key to enter "receive sonic command"; destructive commands (factory reset)
require a second confirmation on the device; bind tokens are single-use and expire in
10 minutes. Use a guest network when provisioning in public. To resist eavesdropping, a
later version can show a one-time code on the device screen and derive a payload
encryption key from it.

## 3. Command Frame

The payload of a sonic frame. Implementations: [`web/sonic/wm-proto.js`](../../web/sonic/wm-proto.js)
and [`main/wm_proto.c`](../../main/wm_proto.c).

```text
payload = [cmd:1][seq:1][TLV...]        TLV = [tag:1][len:1][value:len]
```

| cmd | Name | Required fields | Device behavior |
| --- | --- | --- | --- |
| `0x01` | PROVISION | SSID; optional PASSWORD, BIND_TOKEN | Join Wi-Fi; with a token, claim the binding once online |
| `0x02` | BIND | BIND_TOKEN | Claim the binding when the device is already online |
| `0x03` | SERVER | URL | Store the server URL (development / private deployment) |
| `0x04` | RESET | - | Ask for confirmation, then erase Wi-Fi and binding |
| `0x05` | MESSAGE | TEXT | Show the text on screen |

| tag | Name | Constraint |
| --- | --- | --- |
| `0x01` | SSID | UTF-8, 1..32 bytes |
| `0x02` | PASSWORD | UTF-8, 0..64 bytes; at least 8 bytes when not empty |
| `0x03` | BIND_TOKEN | Printable ASCII, 1..24 bytes |
| `0x04` | URL | Printable ASCII, 1..96 bytes, starting with `http://` or `https://` |
| `0x05` | TEXT | UTF-8, 1..96 bytes |

The device skips unknown tags (forward compatibility) and rejects unknown commands with
an "upgrade the firmware" hint. To add a command, add one constant on each side and one
case in [`tests/test_wm_sonic_interop.mjs`](../../tests/test_wm_sonic_interop.mjs).

## 4. Binding Design

Device identity: `deviceId` is derived from the factory MAC (`WM-` plus 12 hex digits);
`secret` is a 128-bit random value generated when the device first goes online, stored
in NVS and kept across factory resets. The server records `secret` the first time it
sees a device (trust on first use); afterwards the same `deviceId` must present the same
`secret`. On success the server issues a `deviceToken`, the only credential the device
uses for sync. A device binds to a **user account**; rebinding revokes the old token.

**Flow A: one-step sonic binding (recommended, out of the box)**

```text
H5 (signed in)                 Server                         Device
  | POST /api/devices/bind-sessions                             |
  | ---------------------------> | create single-use token      |
  | <----------- {id, token} --- | (10 minutes)                 |
  | sonic PROVISION{ssid, password, token} ------------------>  |
  |                              | <-- POST /device/v1/claim -- | once on Wi-Fi
  |                              | --- {deviceToken, owner} --> |
  | GET /api/devices/bind-sessions/{id} (every 2 s)             |
  | <---- {status: "claimed", device} - |                       | shows success
```

The user does one thing: type the Wi-Fi password and tap send. Failure reasons (wrong
password, network not found, expired token) appear on the device screen; if the H5 page
does not see `claimed` within 2 minutes it tells the user to look at the device.

**Flow B: pairing code (fallback)**

When the device is online but unbound (the claim in flow A failed, it was unbound, or it
is moving to another account), it shows a 6-digit pairing code and a QR code. The user
enters the code on the desktop device page or in H5, or scans the QR code to open the
page with the code filled in:

```text
Device -- POST /device/v1/pairing ------> Server    returns {code, url, expiresIn}
Device -- POST /device/v1/pairing/poll -> Server    every 3 s; pending / bound / expired
User   -- POST /api/devices/claim-by-code {code} -> Server
```

**Unbinding.** Removing the device on the desktop page revokes its token; the device gets
a 401 on its next sync, clears its local binding, and returns to the pairing-code page.
Unbinding from the device menu notifies the server on a best-effort basis and then clears
the local binding (when offline only the local side is cleared; the desktop page can
still remove the device).

## 5. Device API `/device/v1`

Transport: HTTPS with JSON; the device polls. Errors always use
`{"error": {"code": "...", "message": "..."}}`. The device shows `message` on screen as
is, so keep it short and in the user's language. JSON responses must not exceed 8000 bytes; the firmware receive buffer is 8193 bytes.

| Method and path | Auth | Request | Success response |
| --- | --- | --- | --- |
| `POST /claim` | None | `deviceId, secret, model, fw, token` | `deviceToken, owner.name` |
| `POST /pairing` | None | `deviceId, secret, model, fw` | `code, url, expiresIn, interval` |
| `POST /pairing/poll` | None | `deviceId, secret` | `status` = `pending` (with `expiresIn`) / `bound` (with `deviceToken, owner`) / `expired` |
| `GET /sync` | Bearer | Query `rev, bat, rssi, fw, ack` | See below |
| `GET /portrait?revision=…` | Bearer | Revision from sync | Exactly 4608 bytes: 48×48 little-endian RGB565; 404 if absent or changed |
| `POST /unbind` | Bearer | `{}` | `{}` |

`/claim` 4xx codes: `token_invalid` (404), `token_expired` (410), `token_used` (409),
`secret_mismatch` (403). `/sync` and `/unbind` return 401 for an invalid token.

`GET /sync` response:

```json
{
  "rev": 12, "unchanged": false,
  "serverTime": 1794370654, "tzOffsetMin": 480, "nextPollSec": 30,
  "owner": {"name": "..."},
  "card": {"saved": true, "name": "...", "title": "...", "org": "...",
           "contactCode": "BEGIN:VCARD\r\n...", "avatarRevision": ""},
  "badge": {"event": "...", "name": "...", "title": "...", "org": "...",
            "checkinCode": "...", "checkedIn": false},
  "agenda": [{"time": "14:30", "title": "...", "place": "..."}],
  "notices": [{"id": 7, "title": "...", "body": "..."}],
  "command": {"id": 3, "type": "locate", "text": "", "value": 0}
}
```

- `rev`: content version. The device sends the `rev` it holds; when nothing changed the
  server returns only `rev, unchanged: true, serverTime, tzOffsetMin, nextPollSec, command`.
- `serverTime` / `tzOffsetMin`: the device has no RTC and uses these to show the event's
  local time.
- `card`: `saved` indicates an owner-saved business card. UTF-8 limits for `name/title/org` are 180/240/240 bytes. `contactCode` is a complete vCard (up to 1600 bytes, never truncated); `avatarRevision` is a 64-character hexadecimal version or empty. An unsaved card shows the account name/company/portrait, never an inferred login-email contact code.
- Portraits are fetched on revision change or failed download; removal/unbinding clears them. Uploaded and Google account portraits are supported, with no fallback after explicit removal. Cloud and LVGL each own a fixed 4608-byte pixel copy; downloads reuse the JSON buffer and require no device image decoder. Portrait download failures do not block other sync content.
- `badge`: `null` when there is no event to show. Limits in UTF-8 bytes: `event` 63,
  `name` 47, `title` 63, `org` 63, `checkinCode` 127. Longer values are truncated.
- `agenda`: up to 5 entries (`time` 11, `title` 63, `place` 31). `notices`: up to 4,
  newest first (`title` 47, `body` 159), with a monotonically increasing `id`.
- `command`: one pending remote command at a time, resent on every sync until
  acknowledged; the device acknowledges with `ack=<id>` on the next sync. `type`:
  `locate` (beep and wake), `message` (`text`), `brightness` (`value` 20..100), `reboot`,
  `unbind`.
- Query parameters `bat` (battery percent, -1 = unknown), `rssi` (dBm), and `fw`
  (firmware version) feed the desktop page.
- The device syncs every 30 seconds by default and 4 times slower with the screen off;
  the server can adjust this with `nextPollSec` (at least 5).

Displayable text: ASCII, common Chinese and Western punctuation, and every GB2312
ideograph; 16 px text additionally covers the CJK basic block (U+4E00..U+9FA5).
Characters outside this range (emoji, kana, and so on) show as placeholder boxes; the
backend may filter them.

## 6. APIs for H5 and Desktop

These are the endpoints of the reference implementation. A real backend adds sign-in and
workspace permissions, and may rename paths to match WeMeet Next conventions.

| Method and path | Purpose |
| --- | --- |
| `POST /api/devices/bind-sessions` | Start binding from H5; returns `id, token, expiresIn` |
| `GET /api/devices/bind-sessions/{id}` | H5 polling: `pending` / `claimed` (with `device`) / `expired` |
| `POST /api/devices/claim-by-code` | Bind by pairing code |
| `GET /api/devices` | Device list: name, online, battery, signal, firmware, last seen, pending commands |
| `PATCH /api/devices/{id}` | Rename |
| `POST /api/devices/{id}/commands` | Queue a remote command |
| `DELETE /api/devices/{id}` | Unbind |

A device counts as online when its last `/sync` is within 3 sync periods. The reference
implementation also has `POST /api/devices/{id}/notices` and a `checkedIn` field on
`PATCH`; they exist only to demonstrate notices and check-in state. Real data comes from
WeMeet Next events and registrations.

## 7. Integrating into WeMeet Next

- **H5**: copy the two dependency-free ES modules in [`web/sonic/`](../../web/sonic) into
  the H5 project; follow the page flow of [`web/h5.html`](../../web/h5.html). Playback
  must start from a user tap; tell the user to unmute and set media volume to maximum; a
  browser cannot read the current Wi-Fi name, so the user types it (remember the name,
  never store the password).
- **Desktop**: model the device page on [`web/pc.html`](../../web/pc.html): device list,
  add by pairing code, locate / message / rename / unbind. In `apps/web-pc`, build it
  from the shared components and tokens.
- **Backend**: implement sections 5 and 6. Tables needed at minimum: `device` (id, secret
  hash, owner, token hash, name, firmware, last seen, battery, signal),
  `device_bind_session`, `device_pairing`, `device_command`. See
  [`tools/wm_mock_server.mjs`](../../tools/wm_mock_server.mjs) for reference logic.

## 8. Local Development

```bash
node tools/wm_mock_server.mjs          # prints LAN URLs for the phone page, desktop page, and device API
```

1. After flashing, the device rests on the welcome page; press OK to start listening.
2. Open the printed `/h5` URL on a phone and send the mock server URL to the device with
   the "server URL" field (the built-in production default is
   `https://api-global.wemeet.com`, set by `CONFIG_WM_API_BASE`).
3. Enter listening again, fill in the Wi-Fi credentials, tap send, and wait for the
   device to show the binding success page.
4. Open `/pc` on a computer and try locate, message, push notice, and unbind.

To debug the sonic link offline, record what the device hears as a 16 kHz mono s16le raw
file with any tool and decode it with [`tests/wm_sonic_cli.c`](../../tests/wm_sonic_cli.c).

## 9. Known Limitations

- The sonic link is cleartext and one-way; the device sends no acknowledgement to the
  phone (the H5 page learns the result from the server).
- NVS is not encrypted: the Wi-Fi password and device token are stored in flash in the
  clear. Enable flash encryption before mass production.
- Only 2.4 GHz Wi-Fi is supported; captive-portal networks are not.
- OTA updates and deep sleep are not implemented; with the screen off, Wi-Fi stays
  connected in modem-sleep mode.
- The default server URL in the firmware is a placeholder; production builds must set
  `CONFIG_WM_API_BASE` in `sdkconfig`.


## 10. WeMeet Next Integration

WeMeet Next implements personal-account device management (`/#/devices`) and mobile provisioning
(`/m/#devices`) with the device endpoints in this contract. Browser command requests additionally
carry a UUID `requestId` for idempotency. Apply the device migration and deploy the backend before
integration testing. The firmware default URL remains a placeholder; send the actual API origin
using the sonic server command. Reference pages and the mock server are not the production backend.

Content is projected from the nearest published, unfinished event with confirmed participation,
its agenda, and account notifications. The platform has no check-in service yet: `checkinCode` is
empty and the firmware does not imply a pending check-in state. Sync currently always sends a full snapshot.

Before the first HTTPS request, firmware obtains time via SNTP; failure never disables TLS verification.
Remote reboot waits for successful sync carrying that command's acknowledgement instead of a fixed delay.
Full-width 32-byte SSIDs and 64-byte PSKs are copied without truncation. Host policy tests and an
ESP-IDF 5.5.3 build cover these paths. Authorized flashing and write verification succeeded; startup logs confirm display, buttons, audio and battery initialization, with no crash or reset loop in a 25-second window. The user confirmed readable Chinese, OK entering listening mode, and long-press exiting. A nearby Mac speaker transmitted a test message: logs confirmed a 22-byte frame with zero corrected bytes, and the user confirmed the displayed test-success message after replay. Phone-based sonic server setup, Wi-Fi connectivity, code-based pairing, continuing cloud sync, remote messages, locate, and recovery of the binding after remote reboot have been exercised. Brightness commands have execution logs and acknowledgements; their visual effect was not separately confirmed. Automatic Wi-Fi + token binding has passed on the fixed firmware; HTTPS, runtime memory/power, and venue tests remain pending.

During reprovisioning, Wi-Fi configuration changes and driver events share the system event task. The old driver stops before credentials change, and connection starts only on the new start event; stale disconnect/IP events cannot reconnect early. Host regression tests cover the state conflict, consecutive configuration requests, reconnection, and cancellation. Hardware retesting successfully reprovisioned the existing Wi-Fi network; the backend confirmed consumption of the one-time sonic token, and the user confirmed automatic binding without a pairing code.

The updated firmware has been flashed with an application-only write and verified. The user confirmed the business-card/portrait page, contact QR display and return, symmetric battery insets, notice selection/detail/long-press navigation, and the persistent top-left WeMeet label with centered time and unobstructed right-side indicators. Host rendering also verified the battery pixels and decoded the contact QR. Physical phone scanning, runtime memory margin and HTTPS remain separate pending checks.
