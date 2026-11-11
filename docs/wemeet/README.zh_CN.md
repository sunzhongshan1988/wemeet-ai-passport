<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# WeMeet Passport:设备、声波协议与接口契约

WeMeet Passport 是 WeMeet Next 的专属硬件:一块挂在胸前的电子参会证。本文是固件、
H5、PC 端和后端四方对接的唯一契约。固件实现在 [`main/`](../../main),可运行的参考
实现在 [`web/`](../../web) 和 [`tools/wm_mock_server.mjs`](../../tools/wm_mock_server.mjs)。

## 1. 设备做什么

| 能力 | 说明 |
| --- | --- |
| 我的名片 | 第一页显示头像、姓名、职位和公司；OK 展示已保存名片的 vCard 联系人二维码，长按 OK 返回 |
| 日程 | 最多 5 条:时间、标题、地点 |
| 通知 | 保留 4 条、首页预览 3 条；OK 进入列表，上下选中任一条、OK 查看详情，长按逐级返回；详情上下滚动。新通知点亮屏幕并提示 |
| 设备页 | 设备号、绑定账号、Wi-Fi、固件版本;设备菜单(接收声波命令 / 解绑 / 恢复出厂) |
| 声波命令 | 麦克风接收手机 H5 播放的命令:配网、绑定、改服务器地址、恢复出厂、显示文本 |
| 远程管理 | PC 端下发:查找设备、显示消息、调亮度、重启、解绑 |

状态栏左上角固定显示 WeMeet，时间居中（未校时为“--:--”），信号与电池保留在右侧。

按键(上 / 下 / OK):

| 场景 | 上 / 下 | OK 单击 | OK 长按 |
| --- | --- | --- | --- |
| 主界面 | 翻页 | 名片页展示名片码；通知页进入列表；设备页打开菜单 | 进入「接收声波命令」 |
| 引导页 | — | 进入「接收声波命令」 | 同单击 |
| 配对码页 | — | 设备菜单 | 进入「接收声波命令」 |
| 通知列表 | 选择条目 | 进入详情 | 返回通知首页 |
| 通知详情 | 滚动正文 | — | 返回列表并保持选中项 |
| 名片二维码 | — | — | 返回名片 |
| 菜单 | 移动 | 确认 | 返回 |
| 聆听 / 连接中 | — | — | 取消 |
| 提示页 | 继续 | 继续 | 继续 |

无操作 20 秒调暗背光,60 秒息屏;息屏时第一下按键只点亮屏幕。

## 2. 声波链路 WeMeet Sonic v1

H5 用手机扬声器播放,设备麦克风接收,单向传输。实现:发送端
[`web/sonic/wm-sonic.js`](../../web/sonic/wm-sonic.js),接收端
[`main/wm_sonic.c`](../../main/wm_sonic.c)。

| 参数 | 取值 | 原因 |
| --- | --- | --- |
| 调制 | 16-FSK,每符号 1 个音 = 4 bit | 单音包络恒定,手机喇叭可开最大而不削波 |
| 数据频点 | 2000 + 200 × k Hz,k = 0..15 | 手机喇叭最响的频段;30 ms 窗下严格正交 |
| 前导频点 | P0 = 1600 Hz,P1 = 1800 Hz | 与数据频点分开,避免数据被误认为前导 |
| 符号时长 | 40 ms,两端各 5 ms 升余弦过渡 | 接收端只分析中间 30 ms,吞掉混响与切换瞬态 |
| 前导 | 7 符号 Barker 码 `+ + + - - + -`(+ = P0) | 自相关旁瓣小,同步精度 ±5 ms |
| 唤醒音 | 帧前 120 ms 的 1200 Hz | 让手机功放先稳定;不参与解码 |
| 接收采样 | 16 kHz / 16 bit / 单声道 | BSP 的 I2S 固定为 16 kHz |

帧结构(符号内高半字节在前):

```text
[前导 7 符号]
[帧头 6 字节: len, 0xA1, RS 校验 4 字节]            len = payload 长度 + 2
[帧体: payload, CRC16 2 字节, RS 校验 npar 字节]
npar = clamp(2 × ceil(len / 6), 8, 60)
```

- 纠错:GF(2^8) Reed-Solomon,本原多项式 `0x11D`,生成多项式根 α^0..α^(npar-1)。
  帧头可纠 2 字节,帧体可纠 npar / 2 字节(约每 6 字节纠 1 字节)。
- 校验:CRC-16/CCITT-FALSE(多项式 `0x1021`,初值 `0xFFFF`),高字节在前。
- payload 最长 178 字节。速率约 12.5 字节 / 秒:典型配网命令 5 到 7 秒。
- H5 默认把同一帧重播 3 遍,间隔 0.6 秒;设备按 `(cmd, seq, 长度)` 去重。

主机测试覆盖的信道条件(见 [`tests/test_wm_sonic.c`](../../tests/test_wm_sonic.c)):
全带信噪比 0 dB 与 -6 dB、峰值 120 LSB 的小信号、12 ms 回声、±300 ppm 时钟偏差、
120 ms 遮挡、5 分钟纯噪声无误触发。这些是仿真结果,真实手机与会场的表现仍需真机验证。

**安全边界。** 声波是明文广播:在场的人可以录下并解出 Wi-Fi 密码。v1 的缓解措施是:
设备只有在用户按键进入「接收声波命令」后才开麦克风;破坏性命令(恢复出厂)必须在
设备上再按键确认;绑定令牌一次性、10 分钟过期。在公共场合配网请选用访客网络。
如需防窃听,后续版本可在设备屏幕显示一次性验证码并用它派生密钥加密 payload。

## 3. 命令帧

声波帧的 payload。实现:[`web/sonic/wm-proto.js`](../../web/sonic/wm-proto.js)、
[`main/wm_proto.c`](../../main/wm_proto.c)。

```text
payload = [cmd:1][seq:1][TLV...]        TLV = [tag:1][len:1][value:len]
```

| cmd | 名称 | 必需字段 | 设备行为 |
| --- | --- | --- | --- |
| `0x01` | PROVISION | SSID;可选 PASSWORD、BIND_TOKEN | 连接 Wi-Fi;带令牌时联网后自动认领绑定 |
| `0x02` | BIND | BIND_TOKEN | 设备已联网时直接认领绑定 |
| `0x03` | SERVER | URL | 保存服务器地址(联调 / 私有化部署) |
| `0x04` | RESET | — | 弹出确认,按键确认后清除配网与绑定 |
| `0x05` | MESSAGE | TEXT | 在屏幕上显示文本 |

| tag | 名称 | 约束 |
| --- | --- | --- |
| `0x01` | SSID | UTF-8,1..32 字节 |
| `0x02` | PASSWORD | UTF-8,0..64 字节;非空时至少 8 字节 |
| `0x03` | BIND_TOKEN | 可打印 ASCII,1..24 字节 |
| `0x04` | URL | 可打印 ASCII,1..96 字节,以 `http://` 或 `https://` 开头 |
| `0x05` | TEXT | UTF-8,1..96 字节 |

设备跳过不认识的 tag(向前兼容),拒绝不认识的 cmd 并提示升级固件。新增命令时在两侧
各加一个常量,并在 [`tests/test_wm_sonic_interop.mjs`](../../tests/test_wm_sonic_interop.mjs)
里加一条用例。

## 4. 绑定设计

设备身份:`deviceId` 由出厂 MAC 派生(`WM-` + 12 位十六进制);`secret` 是设备首次
联网时生成的 128 位随机数,保存在 NVS,恢复出厂也保留。服务器在设备第一次出现时记录
`secret`(首次使用即信任),之后同一 `deviceId` 必须出示相同的 `secret`。绑定成功后
服务器签发 `deviceToken`,设备此后只用它访问同步接口。设备绑定到**用户账号**;
重新绑定会吊销旧令牌。

**流程 A:声波一步绑定(推荐,开箱即用)**

```text
H5(已登录)                    服务器                         设备
  │ POST /api/devices/bind-sessions                            │
  │ ───────────────────────────► │ 生成一次性 token(10 分钟)  │
  │ ◄─────────── {id, token} ─── │                             │
  │ 声波 PROVISION{ssid, password, token} ───────────────────► │
  │                              │ ◄── POST /device/v1/claim ─ │ 连上 Wi-Fi 后
  │                              │ ─── {deviceToken, owner} ─► │
  │ GET /api/devices/bind-sessions/{id}(每 2 秒)              │
  │ ◄──── {status: "claimed", device} ─ │                      │ 显示「绑定成功」
```

用户只做一件事:填 Wi-Fi 密码并点发送。失败原因(密码错、找不到网络、令牌过期)
显示在设备屏幕上;H5 在 2 分钟内没等到 `claimed` 就提示用户看设备屏幕。

**流程 B:配对码(兜底)**

设备已联网但未绑定时(流程 A 的认领失败、被解绑后、或换账号),屏幕显示 6 位配对码
和二维码。用户在 PC 设备管理页或 H5 输入配对码(或扫码打开已填好的页面):

```text
设备 ── POST /device/v1/pairing ──────► 服务器      返回 {code, url, expiresIn}
设备 ── POST /device/v1/pairing/poll ─► 服务器      每 3 秒;pending / bound / expired
用户 ── POST /api/devices/claim-by-code {code} ──► 服务器
```

**解绑**:PC 端移除设备 → 服务器吊销令牌 → 设备下次同步收到 401,清除本地绑定并回到
配对码页。设备端菜单解绑 → 设备尽力通知服务器后清除本地绑定(离线时只清本地,
PC 端仍可手动移除)。

## 5. 设备接口 `/device/v1`

传输:HTTPS + JSON,设备主动轮询。错误响应统一为
`{"error": {"code": "...", "message": "..."}}`,`message` 会直接显示在设备屏幕上,
请用简短中文。JSON 响应不超过 8000 字节；固件接收缓冲为 8193 字节。

| 方法与路径 | 鉴权 | 请求 | 成功响应 |
| --- | --- | --- | --- |
| `POST /claim` | 无 | `deviceId, secret, model, fw, token` | `deviceToken, owner.name` |
| `POST /pairing` | 无 | `deviceId, secret, model, fw` | `code, url, expiresIn, interval` |
| `POST /pairing/poll` | 无 | `deviceId, secret` | `status` = `pending`(带 `expiresIn`)/ `bound`(带 `deviceToken, owner`)/ `expired` |
| `GET /sync` | Bearer | 查询参数 `rev, bat, rssi, fw, ack` | 见下 |
| `GET /portrait?revision=…` | Bearer | 同步返回的头像版本 | 48×48 RGB565 小端二进制，恰好 4608 字节；无头像或版本变化返回 404 |
| `POST /unbind` | Bearer | `{}` | `{}` |

`/claim` 的 4xx:`token_invalid`(404)、`token_expired`(410)、`token_used`(409)、
`secret_mismatch`(403)。`/sync` 与 `/unbind` 在令牌无效时返回 401。

`GET /sync` 响应:

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

- `rev`:内容版本。设备带上已有的 `rev`;未变化时服务器只返回
  `rev, unchanged: true, serverTime, tzOffsetMin, nextPollSec, command`。
- `serverTime` / `tzOffsetMin`:设备没有 RTC,靠它校时并显示活动当地时间。
- `card`：`saved` 表示是否保存过个人名片；`name/title/org` 的 UTF-8 上限分别为 180/240/240 字节，`contactCode` 为完整 vCard（最多 1600 字节，不截断），`avatarRevision` 为头像版本（64 位十六进制或空串）。未保存名片时只展示账号名称/公司与头像，不自动把登录邮箱放入二维码。
- 头像只在版本变化或下载失败时请求，解绑/移除头像时清除；上传照片与 Google 账号头像均支持，明确移除照片时不回退。网络任务与 LVGL 各保存一份固定 4608 字节像素，JSON 缓冲复用于下载，避免在设备解码大图。头像下载失败不阻塞其他内容同步。
- `badge`:没有可显示的活动时为 `null`。字段上限(UTF-8 字节):`event` 63、`name` 47、
  `title` 63、`org` 63、`checkinCode` 127。超长会被设备截断。
- `agenda` 最多 5 条(`time` 11、`title` 63、`place` 31);`notices` 最多 4 条、新的在前
  (`title` 47、`body` 159),`id` 单调递增。
- `command`:待执行的远程命令,一次一条,未回执前每次同步都重发;设备执行后在下一次
  同步用 `ack=<id>` 回执。`type`:`locate`(响铃亮屏)、`message`(`text`)、
  `brightness`(`value` 20..100)、`reboot`、`unbind`。
- 查询参数 `bat`(电量 %,-1 = 未知)、`rssi`(dBm)、`fw`(固件版本)供 PC 端展示。
- 设备默认每 30 秒同步一次,息屏后放慢 4 倍;服务器可用 `nextPollSec`(≥ 5)调整。

文字显示范围:ASCII、常用中英文标点、GB2312 全部汉字;16 px 文字另覆盖 CJK 基本区
(U+4E00..U+9FA5)。范围外的字符(emoji、假名等)显示为占位框,后端可按需过滤。

## 6. 面向 H5 / PC 的接口

下表是参考实现里的接口,真实后端应加上登录态与空间权限,路径可按 WeMeet Next 的
规范调整。

| 方法与路径 | 用途 |
| --- | --- |
| `POST /api/devices/bind-sessions` | H5 发起绑定,返回 `id, token, expiresIn` |
| `GET /api/devices/bind-sessions/{id}` | H5 轮询:`pending` / `claimed`(带 `device`)/ `expired` |
| `POST /api/devices/claim-by-code` | 输入配对码绑定 |
| `GET /api/devices` | 设备列表:名称、在线、电量、信号、固件、最近在线、待执行命令数 |
| `PATCH /api/devices/{id}` | 重命名 |
| `POST /api/devices/{id}/commands` | 下发远程命令 |
| `DELETE /api/devices/{id}` | 解绑 |

「在线」的判定:最近一次 `/sync` 在 3 个同步周期内。参考实现里另有 `POST /api/devices/{id}/notices`
和 `PATCH` 的 `checkedIn` 字段,仅用于演示通知与签到状态;真实数据来自 WeMeet Next 的活动与报名。

## 7. 接入 WeMeet Next

- **H5**:把 [`web/sonic/`](../../web/sonic) 两个零依赖 ES module 拷进 H5 工程;页面
  流程参考 [`web/h5.html`](../../web/h5.html)。必须由用户点击触发播放;提示用户取消
  静音并把媒体音量调到最大;浏览器读不到当前 Wi-Fi 名,需要用户输入(可记住名称,
  不要保存密码)。
- **PC**:设备管理页参考 [`web/pc.html`](../../web/pc.html):设备列表、配对码添加、
  查找 / 发消息 / 重命名 / 解绑。落到 `apps/web-pc` 时使用其公共组件与 token。
- **后端**:实现第 5、6 节;数据表至少需要 `device`(id、secret 哈希、owner、
  token 哈希、名称、固件、最近在线、电量、信号)、`device_bind_session`、
  `device_pairing`、`device_command`。参考逻辑见
  [`tools/wm_mock_server.mjs`](../../tools/wm_mock_server.mjs)。

## 8. 本地联调

```bash
node tools/wm_mock_server.mjs          # 打印手机页、PC 页与设备 API 的局域网地址
```

1. 烧录固件后,设备停在引导页;按 OK 进入「正在聆听」。
2. 手机打开打印出的 `/h5` 地址,先用「服务器地址」把模拟服务器地址发给设备
   (固件默认地址是占位符 `https://api.wemeet.example`,由 `CONFIG_WM_API_BASE` 配置)。
3. 再次进入聆听,填写 Wi-Fi 并点「发送到设备」,等待设备显示「绑定成功」。
4. 电脑打开 `/pc` 页面,试用查找、发消息、推送通知、解绑。

离线排查声波问题:用任意工具把设备听到的声音录成 16 kHz 单声道 s16le 原始文件,
交给 [`tests/wm_sonic_cli.c`](../../tests/wm_sonic_cli.c) 解码。

## 9. 已知限制

- 声波为明文、单向;设备不向手机回传确认(H5 通过服务器得知结果)。
- NVS 未加密:Wi-Fi 密码与设备令牌以明文保存在 Flash。量产前应启用 Flash 加密。
- 只支持 2.4 GHz Wi-Fi;不支持需要网页登录的认证网络。
- 尚未实现 OTA 升级与深度休眠;息屏后 Wi-Fi 仍保持连接(调制解调器省电模式)。
- 固件默认服务器地址是占位符,正式环境需在 `sdkconfig` 里配置 `CONFIG_WM_API_BASE`。


## 10. WeMeet Next 实际接入

WeMeet Next 已提供个人账号设备页（PC `/#/devices`、手机 `/m/#devices`）和本契约的设备接口。
浏览器命令请求另带 `requestId` UUID，供后端幂等处理。后端需应用设备迁移并部署后才可联调；
本仓库默认地址仍为占位符，可通过声波设置实际 API 地址。参考页面和模拟服务器不是正式后端。

正式数据由已确认报名、已发布且未结束的最近活动、议程和通知投影；平台目前没有签到能力，
此时 `checkinCode` 为空，固件不展示“待签到”状态。同步目前总返回完整快照。

固件首次 HTTPS 请求前通过 SNTP 校时；校时失败不会跳过 TLS 校验。远程重启等待携带该命令
回执的同步成功，不再依赖固定延时。32 字节 SSID 和 64 字节 PSK 按驱动完整缓冲复制。
这些路径有主机规则测试与 ESP-IDF 5.5.3 编译验证。已授权烧录并通过写入校验，启动日志确认显示、按键、音频和电量计初始化成功；25 秒观察未发现崩溃或复位循环。用户实测确认中文正常、OK 可进入聆听、长按可退出。Mac 扬声器近距离发送测试消息，日志确认解码 22 字节、纠错 0 字节，用户在重播后确认屏幕显示测试成功。手机声波设置服务器、Wi-Fi 联网、配对码绑定、持续云端同步、远程消息、查找和重启后恢复绑定已实测；亮度命令有执行日志与回执，视觉效果未单独确认。修复版已验证 Wi-Fi + 令牌自动绑定；HTTPS、运行时内存/功耗和会场验证仍待完成。

重复配网时，Wi-Fi 配置变更与驱动事件统一在系统事件任务处理。先停止旧驱动，再设置凭据并等待新启动事件连接；旧连接的断开或 IP 事件不能抢先重连。主机回归测试覆盖这一状态冲突、连续配置请求、断线重试和主动取消。修复版真机复测已在原有 Wi-Fi 上重新配网成功，后台确认一次性声波令牌被认领，用户确认无需配对码便自动绑定。

新版已仅更新程序区域并校验写入。用户已确认名片/头像页面、名片码显示与返回、电池内芯对称留白、通知选择/详情/长按返回均正常；左上固定 WeMeet、居中时间和右侧图标也已确认没有重叠。主机实际绘图验证了电池像素与名片码解码；手机实机扫码、运行内存余量及 HTTPS 仍需专项验证。
