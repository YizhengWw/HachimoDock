# ESP-P4 USB Protocol

0.7.103-p4：内置清单、启动器图片及完整出厂资源统一为 29 项。末尾追加 `decision-capsule`、`answer-book`、`hope-train`；通用 `widgetLocalTools=p4-local-tools-v1` 契约保持不变。OTA 仍保留轻量资源引导与现有用户存档，完整出厂镜像才携带全部图片。

0.7.100-p4：新增 `widgetMediaQueueMax=128` 与 `widgetMediaLifecycle=p4-media-lifecycle-v1`。媒体队列超过 20 项时，key 至多 80 UTF-8 字节、标题至多 90 字节；上限 128，刷新按 key 保留选中项，不修改音频会话。`media/event` 新增 `operation=visibility`，包含 `source/boot/epoch/visible`；进入或离开递增 epoch，可见时每 2 秒发送心跳。宿主只有资源包声明自动播放、当前可见且新的进入事件时才准备播放，停止/离开取消未开始的播放；目录刷新不重新触发播放。能力与配置为通用媒体机制，不绑定特定组件 ID。

0.7.83-p4 媒体重入：从组件中心重新进入当前已验证的媒体组件只恢复界面，不重写目录或失效图片缓存，避免后台音频因同步 Flash 操作欠载。切到其他组件、安装更新及游戏重开仍走原有加载/校验路径；媒体会话与全局退出语义不变。

0.7.82-p4 发送约束：运行时日志与 JSON 协议必须在同一传输锁内进入同一 UART/USB 驱动队列；不得混用直接 console FIFO 写入与驱动排队。媒体/实时音频回执包含完整换行后立即返回，不等待队列排空；日志限长且保留换行。不以麦克风采集状态决定协议完整性保护，电台播放及启停过渡同样受保护。

组件中心（0.7.98）：出厂 SPIFFS 完整预置 25 项，顺序由 `factory-config.json` 决定，前四项为 `stock-watchlist`、`wooden-fish`、`upcoming-todos`、`computer-status`；停用、下架组件不重新启用。容量上限 32 项，目录 JSON 上限 16383 字节，单 widget 上限 8191 字节。OTA 内只保留轻量启动目录，不覆盖设备已有完整包；新机完整镜像包含全部贴图，已有设备通过 PC 同步完整资源。目录页使用 2×2 卡片、缓存材质背景与小缩略图，轻点打开、左右滑动翻页；摇杆选择与确认/全局返回保留。PC 宿主服务、公开知识库不搬入设备固件，仍需要 PC 在线。

## PC 音频播放器（0.7.66-p4）

0.7.69-p4：离开随身听组件允许后台播放。在宠物主页触发配置的全局 `page_back` 时，设备立即停止音乐并清空音乐缓冲，上报 `media/event {operation:"stop"}`，同时 input/event 标记 action=media_stop、handledLocally=true。即使 PC 尚在准备歌曲也上报，以取消对应任务；PC 不在接收线程阻塞等待 USB，补发的清理只针对当时的音乐会话。保留曲库，不自动下一首；实时对话中的退出优先于停止音乐。

能力 `widgetMedia=p4-media-v1` 表示支持 v4 工具的 `media.source=audio.player` 原生播放器。
`media/begin/chunk/end/query/control/stop/metadata/cover/library` 的每个请求使用唯一 requestId，
回复 `media/status` 包含同一 requestId、ok、sessionId、state、positionMs、bufferedMs、volume。
begin 声明 music- 前缀会话、sampleRate=48000、format=ima-block-v1、durationMs/offsetMs/volume。
begin/library 的 queue 与 keys 是同长度、最多 20 项的标题与稳定曲目 ID 数组，全部校验后才替换。
设备 `media/event` 携带 operation，选择歌曲携带 key，不依赖易变化的列表序号。

chunk 携带匹配 sessionId、从 0 单调递增的 seq 和独立 IMA ADPCM base64 块：
LE16 初始采样值、步长索引、零保留字节、LE16 样本数、低 nibble 优先数据；
最多 3840 个样本/1926 字节，48 kHz 单声道约 80 ms。旧会话或重复序号不写入音频。
cover 为 192×192 RGB565 LE，offset 连续且每片不超过 2048 字节；封面失败不阻断音乐。
进度来自实际播放字节，400 ms 缓冲起播（短曲收到 end 可提前播放），PC 有界补充。
语音优先：开始采集前暂停音乐并恢复双向 16 kHz codec；不将音乐混入会话 AEC。
媒体控制/数据请求维护 5 秒失联停止租期，library 刷新不续期。音乐不写 flash。
详细 PC 服务、技能和测试边界见 [播放器说明](../pc/docs/music-player.md)。

## PC 推送的组件实时数据（0.7.60-p4）

能力 `widgetData: "p4-data-list-v1"` 表示支持 `widget/data` 完整快照。
payload 为 `schema/source/ttlMs/status/message/rows`；最多 20 行、每页 5 行，
4 个命名数据源保存在 RAM。校验全部成功才替换，失败不续期；有效期使用设备单调时钟。
0.7.61-p4 起可选 `date` 字段接收 `YYYY-MM-DD`，只在列表顶部绘制；省略时保持旧发送端兼容。每行名称和元信息保持普通颜色，仅 value/detail 使用 tone。
列表组件在 v4 runtime 声明 `data.source` 与整数 `data.page_var`，无需修改旧组件。
PC 在文件传输占锁时推迟快照，过期数据明确提示，不启用设备网络。
完整字段约束、股票提供器与隐私边界见 [实时数据说明](../pc/docs/widget-live-data.md)。

## Transport

The P4 runtime keeps one JSON object per line over the ESP32-P4 USB-UART path
for flashing, rescue, and early logs. When the board Type-C jumper is switched
to the ESP32-P4 native USB OTG path, runtime data should use the high-speed
vendor bulk protocol `pet-usb-native-v1` instead.

JSON messages still use the same outer shape as the current Pet Manager USB
transport:

```json
{"topic":"<topic>","payload":{}}
```

## Wi-Fi Transport (`pet-net-jsonl-v1`)

Boards with the ESP32-C6 companion radio can carry the same JSON-per-line business protocol over the
LAN. Wi-Fi is a **second link beside USB**, not a replacement: phase one synchronizes status,
sessions and key events only.

**Provisioning (USB only).** `network/config {ssid, password, secret, requestId}` stores the
credentials and a 64-hex pairing secret in NVS; `network/clear` erases them; `network/query` reads
state. Config and clear are refused on the LAN with `usb_required`. Replies are `network/status`
(`requestId`, `ok`, `state` = `unconfigured|idle|starting|connecting|connected|failed`, `ssid`, `ip`,
`rssi`, `port`, `client`, `lastError`, `disconnectReason`, `coprocessorVersion`). `lastError` is one of
`ap_not_found`, `wrong_password`, `disconnected`, `connect_failed`, `c6_unavailable`, `crash_loop`.
Credentials and the secret are never logged or echoed.

**Lifecycle.** Without credentials the radio and the ESP-Hosted transport are never touched; the
host component's boot-time initialization is wrapped to a no-op (`-Wl,--wrap=esp_hosted_init`) and
runs only after configuration, 12 s after boot (past the OTA validation window). Three consecutive
crashes during bring-up set `crash_loop` and disable the radio until the next `network/config`.

**Connection.** TCP `38517`, one authenticated client. UDP `38517` answers
`{"topic":"net/discover","payload":{"nonce"}}` with `net/announce {boardDeviceId, nonce, port, fw,
paired}` (no secrets). Handshake: board sends `net/challenge {boardDeviceId, nonce}`; the desktop
answers `net/auth {desktopDeviceId, nonce, proof}` with `proof = HMAC-SHA256(secret_hex_ascii,
"pet-net-v1|client|<board>|<device nonce>|<client nonce>")`; the board verifies, replies
`net/auth_ok {proof}` using role `server`, and the desktop verifies it before sending `bind`, which
returns the Wi-Fi `hello`. Five failures within 60 s lock authentication for 30 s (`locked`). The board
sends `net/keepalive` every 3 s and drops a client silent for 15 s; the desktop drops a link silent
for 12 s and reconnects with a fresh `bind`/snapshot. The stream is **authenticated but not
encrypted**: use trusted networks.

**Allowed topics.** Desktop to board: `ack, bind, system/heartbeat, speech/text, session/current,
control/screen-page, control/command, stats/update, runtime/stats, state/*, diagnostics/*, network/*`.
Anything else is answered `protocol/ack {ok:false, code:"wifi_unsupported"}`. Board to desktop
broadcasts: `input/*, session/*, protocol/*, diagnostics/*, network/*, button-config-ack`; audio,
media, asset and firmware traffic never leaves over Wi-Fi even when a button starts it.

**Hello over Wi-Fi.** `link:"wifi"`, `capabilities.transport = {kind:"wifi", usbOnly:false,
operations:[state, session, inputEvents, diagnostics]}`, and asset/appearance/widget/audio/voice/
screenshot/firmware capabilities reported false. Every hello carries `capabilities.network`
(`wifi`, `protocol`, `port`, `provisioning:"usb"`, `configured`, `state`). Assets, audio, widgets and
OTA keep their USB transactions. Only one link is the desktop's business channel at a time; USB wins.

## Handshake

### Duplex audio capability extension

`features.audioAec`, `audioVad`, `audioFullDuplex` are true only when the
ESP-SR microphone/reference pipeline initializes. A compatible client requests
`audio/conversation` with `halfDuplex:false`; missing capability retains the
legacy half-duplex behavior. `audio/status` reports live AEC availability.
Processed `audio/chunk` carries `aec:true` and boolean `vadSpeech` alongside
the unchanged 640-byte PCM/checksum fields. Raw capture is never marked AEC.
Runtime AEC failure emits `aec_fallback` and gates playback capture; the PC
stops a duplex session rather than recognizing raw loudspeaker echo.
`features.audioAecRecovery:true` additionally advertises capture-owner recovery:
the client may request duplex even when a previous fault made live availability
false. Each new `audio/conversation` entry attempts DSP preparation once, not on
every frame. `audio/status.audioAecPreparing:true` is a transitional state, not
an AEC failure; the client waits up to 3 seconds before playing its greeting.
`audio/status.aecReason` and `audio/diagnostic.aecReason` use fixed codes:
`none`, `init_failed`, `stream_reset_failed`, `reference_overflow`,
`processed_queue_overflow`, `dsp_too_slow`. A runtime fault stops capture without
uploading raw echo; a new conversation can rebuild the DSP without rebooting.

Firmware 0.7.74 adds numeric DSP diagnostics to status/diagnostic responses:
`dspFrames`, `dspLastUs`, `dspMaxUs`, `dspAecUs`, `dspNsUs`, `dspVadUs` and
`aecReferenceSamples`. Durations are microseconds; frame counts and maxima
reset on a new DSP generation. Stage durations describe the last processing
call, not a session average. These fields contain no audio or transcript and
are not repeated on every PCM chunk. v3 uses FD_HIGH_PERF + VERYAGGR, while
v1 retains VOIP_LOW_COST. USB frames remain 20 ms in both cases.

Audio messages carry `boardDeviceId`. Conversation status, begin, diagnostics,
errors and chunks also carry `conversationSessionId`, echoing the control
request's `sessionId` (maximum 63 bytes). This is separate from the existing
capture `sessionId`. Chunks bind to the conversation snapshot before the codec
read, so a PTT/conversation switch cannot relabel old PCM. Clients discard
another board's audio and stale conversation timelines. Older firmware without
these fields remains supported; raw PCM is never accepted in duplex mode,
and startup readiness has a bounded timeout.
Late `audio/play_chunk` and `audio/play_end` for an inactive playback session
are ignored after cancellation; they cannot append to a new reply. See
`pc/docs/realtime-duplex.md` for acoustic acceptance criteria.

Firmware 0.7.51 reports the actual silicon revision as an integer
`major * 100 + minor` in `capabilities.firmwareUpdate.chipRevision` and in
`diagnostics/status.runtime.chipRevision`. It is not inferred from the board SKU.
The desktop selects the v1 or v3 application image from these values. Both host
operating systems use the same 4M transport and existing chunk/pacing rules.

`firmware/begin` may include `chipRevisionMin` and `chipRevisionMax`. The device
checks them before opening the OTA partition, then independently validates the
first application's ESP image header before writing. Images must explicitly
bound one family (pre-v3 below 200, or v3 300–399); unbounded or mixed-family
images are rejected. Older desktop clients may omit the begin fields, but the
image-header check still applies. New desktop clients reject unknown chip
identity before starting an update; only diagnostics identifying the existing
pre-v3 SDK runtime allow the legacy v1 migration path.

Board to PC:

```json
{
  "topic": "hello",
  "payload": {
    "boardDeviceId": "p4-a1b2c3d4e5f6",
    "runtime": "esp-p4",
    "deviceModel": "ESP32-P4 RISC-V Dual-Core + ESP32-C6",
    "fw": "0.7.42-p4",
    "buildId": "0.7.42-p4+290f402abcd1",
    "gitSha": "290f402abcd1",
    "buildDirty": false,
    "protocolSchema": 6,
    "wireProtocol": "pet-usb-jsonl-v3",
    "hardware": {
      "soc": "ESP32-P4 RISC-V Dual-Core",
      "hpCpu": "HP@360MHz(Max 400MHz)",
      "lpCpu": "LP@40MHz",
      "onChipSram": "768KB L2MEM + 32KB LP SRAM + 8KB TCM",
      "psram": "32MB In-package stacked",
      "flash": "32MB QSPI NOR Flash"
    },
    "capabilities": {
      "version": 1,
      "usbOnly": true,
      "nativeUsb": true,
      "nativeProtocol": "pet-usb-native-v1",
      "bulk": true,
      "maxAppearanceSlots": 2,
      "appearanceSlotReuse": true,
      "rawAppearanceSlot1": true,
      "transport": {
        "usbOnly": true,
        "usbUart": true,
        "usbUartBaud": 4000000,
        "rawAssetChunks": true,
        "rawAssetChunkBytes": 65536,
        "nativeUsb": true,
        "bulk": true,
        "nativeProtocol": "pet-usb-native-v1"
      },
      "display": {
        "width": 640,
        "height": 480,
        "physicalWidth": 480,
        "physicalHeight": 640,
        "pixelFormat": "rgb565"
      },
      "assetFormats": ["p4-h264-v1", "p4-mjpeg-v1", "p4-pcm-wav-v1"],
      "mp4": false,
      "widgets": true,
      "widgetDelete": true,
      "widgetInventory": true,
      "componentCatalogGeneration": true,
      "widgetRuntime": "p4-bounded-runtime-v4",
      "widgetRuntimes": ["p4-bounded-runtime-v3", "p4-bounded-runtime-v4"],
      "widgetScene": "p4-grid-scene-v2",
      "widgetScenes": ["p4-grid-scene-v1", "p4-grid-scene-v2"],
      "widgetSprites": true,
      "componentCatalogMax": 16,
      "widgetGames": ["blocks", "snake", "flappy"],
      "widgetGamePresets": ["blocks", "snake", "flappy"],
      "widgetLimits": {
        "maxVars": 8,
        "maxStates": 6,
        "maxPages": 4,
        "maxTransitions": 12,
        "maxTicks": 8,
        "maxButtons": 8,
        "maxSceneEntities": 12,
        "maxSceneRules": 20,
        "maxSceneOpsPerRule": 4,
        "maxSceneSprites": 4,
        "maxSceneSpritePixels": 4096,
        "maxWidgetJsonBytes": 4095,
        "maxButtonsJsonBytes": 2047,
        "fetchers": false,
        "readers": false
      },
      "voice": true,
      "stats": true,
      "inputs": true,
      "inputConfig": true,
      "audio": true,
      "audioCapture": {
        "codec": "ES8311",
        "format": "pcm_s16le",
        "sampleRate": 16000,
        "channels": 1,
        "bitsPerSample": 16,
        "frameMs": 20,
        "transport": "usb-jsonl-pcm-v1",
        "maxCaptureMs": 30000,
        "playback": true
      },
      "touch": true,
      "touchInput": {
        "controller": "GT911",
        "ready": true,
        "coordinates": "logical-640x480",
        "tap": true,
        "longPress": true,
        "swipe": true
      },
      "firmwareOta": true,
      "firmwareUpdate": {
        "transport": "usb-jsonl-ota-v1",
        "slots": 2,
        "maxImageBytes": 2621440,
        "chunkBytes": 4096,
        "checksum": "sha256",
        "rollback": true,
        "validationMs": 8000,
        "projectName": "pet_manager_p4_runtime"
      },
      "protocolAck": true,
      "screenshot": true,
      "appearance": {
        "formats": ["p4-h264-v1", "p4-mjpeg-v1", "p4-pcm-wav-v1"],
        "maxSlots": 2,
        "slotReuse": true,
        "rawSlot1": true,
        "builtinSlot": 0,
        "customSlot": 1,
        "builtinProtected": true,
        "mp4": false,
        "audio": true
      },
      "features": {
        "mainPage": true,
        "statsPage": false,
        "screenshot": true,
        "controls": true,
        "widgets": true,
        "widgetDelete": true,
        "widgetInventory": true,
        "miniAppPage": true,
        "miniAppState": true,
        "componentCenter": true,
        "voiceCapture": true,
        "audioPlayback": true,
        "touchInput": true,
        "touchGestures": true,
        "firmwareOta": true,
        "protocolAck": true
      },
      "controls": {
        "sw1": 50,
        "sw2": 49,
        "sw3": 5,
        "encoderPress": 4,
        "encoderB": 3,
        "encoderA": 2,
        "activeLow": true,
        "configPersistent": true,
        "touchscreen": true
      },
      "screenPages": ["main", "components", "app"]
    }
  }
}
```

`p4-bounded-runtime-v4` / `p4-grid-scene-v2` add modern semantic shapes and
bounded animated sprites while the advertised arrays preserve v3/v1 package
compatibility. A package may declare at most four horizontal PNG sprite sheets,
eight frames per sheet, and 4096 decoded pixels across all frames. Pet Manager
compiles each source PNG to `runtime/sprites/<id>.p4s` (RGB565 little-endian plus
alpha) before USB downlink. Sprite files are checksum-verified, committed in the
same dual-generation component transaction as `runtime/widget.json` and
`buttons.json`, and restored after reboot; raw PNG files are never interpreted
by firmware.

Hardware-dependent flags are emitted from successful initialization rather
than board-model assumptions. Capture prefers ES7210 and falls back to the
ES8311 ADC/DAC path; a board reports `voice=false` only when neither capture
path can initialize.

`fw` comes from the ESP-IDF application descriptor. `buildId` combines that
version with the 12-character Git commit and appends `-dirty` for a build made
from modified sources. `protocolSchema` is the PC/P4 compatibility contract;
schema `5` adds the ESP32-P4 four-direction joystick events while retaining
the legacy encoder event aliases, explicit build identity, revisioned Session,
transactional component-catalog, and A/B firmware contracts. Older firmware
may omit these fields, in which case Pet Manager reports an unknown build
instead of guessing from the semantic version alone.

## Native USB Bulk

Native USB device mode enumerates as vendor bulk device `0x303A:0x4040`
and exposes a Microsoft OS 2.0 `WINUSB` compatible-ID descriptor. Windows
8.1+ therefore binds the inbox WinUSB driver without a project-specific INF
or Zadig step; macOS/Linux continue to use their native libusb backends.
(`OpenClaw P4 Native USB`). It exposes one bulk OUT endpoint for PC-to-board
frames and one bulk IN endpoint for board-to-PC JSON replies. This is the
preferred transport for large P4 appearance packs.

Frame header, little-endian:

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | magic `P4BU` |
| 4 | 1 | version `1` |
| 5 | 1 | kind |
| 6 | 2 | flags, currently `0` |
| 8 | 4 | sequence |
| 12 | 4 | payload length |

Kinds:

- `1` JSON: payload is the existing UTF-8 JSON message without the trailing newline.
- `2` file begin: payload is JSON metadata with `transferId`, `path`, `size`, and `checksum`.
- `3` file data: payload is raw binary file bytes.
- `4` file end: payload is optional JSON metadata.
- `5` commit: payload is JSON metadata with `transferId`, `fileCount`, and `totalBytes`.
- `6` identity ping: payload is UTF-8 JSON containing a fresh, unpredictable
  `nonce`. The board replies on kind `1` with `native/pong`, echoing that nonce
  and its immutable `boardDeviceId`:

```json
{"nonce":"<host challenge>"}
```

```json
{
  "topic": "native/pong",
  "payload": {
    "protocol": "pet-usb-native-v1",
    "boardDeviceId": "p4-a1b2c3d4e5f6",
    "nonce": "<host challenge>",
    "protocolSchema": 6,
    "buildId": "0.7.42-p4+0123456789ab"
  }
}
```

Before any mutating native operation, the desktop enumerates every matching
VID/PID candidate, challenges each one, and selects the single response whose
`boardDeviceId` exactly equals the requested board. It then reopens and
challenges that candidate again before returning the transport. Missing IDs,
stale/mismatched nonces, unsupported protocols, no match, or duplicate devices
claiming the same ID all fail closed before a slot or file is modified. Firmware
that predates this identity response can still use USB-UART but cannot receive
native writes.

UART and USB Serial/JTAG keep independent framing buffers, and request replies
are written only to the ingress that supplied the command. Unsolicited runtime
events are broadcast to UART, native USB, and USB Serial/JTAG after that path
has sent at least one valid byte. UART, USB Serial/JTAG, and native generic JSON
commands enter one bounded protocol queue; its worker owns JSON parsing and
runtime-state mutation. Native bulk file frames remain serialized by the same
state mutex. The main loop copies a stable render snapshot while holding that
mutex, then releases it before H.264 decode, overlay drawing, rotation, and LCD
refresh, so rendering cannot starve command ACKs.

Normal boot uses `format_if_mount_failed=false`. A damaged or mismatched SPIFFS
volume is preserved and reported as a mount failure; only an explicit factory
recovery/erase operation may format or replace it.

Appearance slot `0` is the protected factory Westie pack. All non-built-in
appearance transfers target replaceable slot `1` and atomically switch the
active marker only after commit. Before `asset/begin` is acknowledged,
firmware clears slot `1` using a fresh SPIFFS directory scan after every
deletion; cleanup failure rejects the transfer instead of leaving skipped stale
files to consume the next pack's capacity. This path never erases protected
slot `0`.
When the active slot contains a valid pre-ready-marker manifest with a
deterministic `packId`, startup promotes that same slot by writing its ready
marker. It does not clear either slot during migration.
Every current manifest carries a deterministic SHA-256 `packId` over its
non-manifest payload. `asset/slot-query` returns the active slot and the
validated `packId` stored in each slot. If the requested pack is already in the
inactive slot, `asset/activate` with the exact `slot` and `packId` switches the
marker and reloads that manifest without rewriting its files. Activation is
rejected while another appearance/firmware transfer or reboot is active.
Firmware without the advertised `appearanceSlotReuse` capability keeps using
the full transactional transfer path. When `rawAppearanceSlot1=true`, every
cache-miss full sync targets the dedicated raw slot `1`. `asset/abort` closes
any open file, invalidates incomplete raw metadata, clears inactive staging,
releases the runtime transfer gate, and replies with `asset/ack` phase `abort`;
the previously active appearance pack remains valid.
If either serial or native-USB appearance traffic stops for 15 seconds before
commit, firmware applies the same inactive-slot cleanup locally and releases
the render gate. This is a crash/disconnect safeguard; normal host failures
still send `asset/abort` immediately.

PC to board:

```json
{"topic":"ack","payload":{"desktopDeviceId":"desktop-..."}}
```

## Supported Input Topics

- `state/<agent>`: updates current lifecycle state, event, and status marker.
- `speech/text`: updates the speech bubble title, body, status, and status text.
  A terminal `done` lifecycle remains visible on the device for 60 seconds
  before the runtime returns it to `idle`, so completed replies stay readable.
- `session/current`: updates the live desktop conversation queue, selected
  position, and temporary explicit Session binding. The main page renders up
  to three cards around the selection. Each item carries `id`, `title`,
  `content`, `state`, `transitionRevision`, and `terminalRemainingMs`; later
  `speech/text` updates use `sessionId` to replace
  the matching card's progress content. Titles use one line and progress uses
  up to two lines, with UTF-8-safe pixel-width `...` truncation. Holding the
  configured `voice_ptt` button replaces the selected card with a waveform.
  When the main page has no Session cards, the same recording feedback appears
  in a standalone bottom panel instead of the idle bubble. Queue refresh,
  expiration, or clearing does not end a physical voice hold; releasing the
  button removes the panel and restores the normal idle bubble.
  The desktop reconciles adjacent snapshots and publishes at most eight cards;
  no manual card-count setting is involved. A cold snapshot admits only active
  lifecycle states. Historical `done`, `error`, and `idle` Sessions remain in
  the independent desktop routing list and never enter the device queue.
  A Session that was active in the immediately preceding visible queue may
  transition to `done` or `error` and remain for exactly 60 seconds. Ordered
  lifecycle events may omit unrelated active Sessions, but each periodic full
  Agent scan is authoritative: an active Session missing from that snapshot is
  removed instead of being retained indefinitely. An explicit `idle` transition
  also removes it immediately. Hidden, manually cleared, and cross-Agent cards are
  not eligible for later terminal retention.
  The desktop assigns a stable, JSON-safe `transitionRevision` whenever the
  lifecycle changes. Terminal snapshots carry the remaining PC-side deadline,
  clamped to `0..60000` milliseconds, rather than starting another full hold.
  Firmware ignores older revisions and preserves the original deadline for a
  repeated revision. A newer terminal revision uses only the supplied remaining
  TTL. This also lets a rebooted board restore a still-visible terminal card
  without extending it. Legacy terminal items without revision metadata still
  require the same `agentId` and a matching active ID in the previous on-device
  queue. Unknown and idle items are rejected, and firmware never
  creates a fallback idle card from the background routing title. Once the last
  retained terminal card expires, the renderer returns to `休息中`.
  `activeSessionIds` separately carries the current bounded active set. Across
  accepted lifecycle states, incoming refreshes update matching cards in place
  instead of adopting the desktop's changing `lastModified` order. First-seen
  relative order therefore remains stable; new visible Sessions append, and
  only hidden or expired cards are removed.
  While this queue is nonempty, the selected card's lifecycle is also the
  authority for appearance and lifecycle audio selection. Aggregate
  `state/<agent>` values remain available for protocol bookkeeping, but cannot
  make the visible working Session play another Session's idle or terminal clip.
  Firmware additionally removes active cards when no `session/current` snapshot
  arrives for 12 seconds; already terminal cards keep only their original
  remaining 60-second deadline. This protects the screen from stale working
  cards if the desktop bridge stops publishing while other USB traffic remains.
  On `main`, device previous/next actions update the selected Session card and
  report its exact ID back to the desktop. On `components`, the same actions
  select the previous/next catalog entry without changing desktop Session
  routing. SW2's default `component_center` action toggles the two peer pages.
  On a followed-Agent change, the desktop sends the new `agentId` with an empty
  `sessions`/`activeSessionIds` snapshot before loading that Agent's queue.
  Firmware treats this as an immediate cross-Agent clear. `displayEnabled:
  false` explicitly clears the visible queue. Older senders that omit the field
  default to display enabled.
  Host activity received concurrently with a render frame is treated as online
  even when that frame captured its timestamp just before the activity arrived.
- `control/screen-page`: switches the P4 renderer between `main`, the on-device
  `components` catalog, and the active bounded mini-app `app` page.
  Other values receive an `invalid_page` protocol NACK.
- `stats/update`: updates the bounded P4 statistics model. The same token and
  runtime metrics are also extracted from `state/<agent>` payloads. For the
  built-in Token dashboard, Pet Manager filters to the current followed Agent,
  aggregates its local-calendar-day Session usage as `dailyTokenUsage`, and
  normalizes that object into the bounded `tokenUsage` field on a dedicated
  `runtime/stats` message before Session-specific state routing is applied.
  Bridge startup publishes this aggregate through a display-free idle snapshot,
  so opening Pet Manager does not require a new Agent message before data appears.
  Activating a mini-app replays the latest bounded stats model immediately, so
  a Token dashboard opened after the telemetry arrived never waits for another update.
  Cross-day Codex rollouts subtract the exact local-midnight cumulative baseline
  rather than exposing lifetime Session usage.
- `input/config`: validates and persists the P4 input map. The compatibility
  envelope `control/command` with `type: button_config` is also accepted. While
  a component is open, its mapped gameplay action wins; the current global
  `page_back` gesture is the only system navigation allowed to escape it.
  Other unmapped system-navigation actions such as `component_center` are
  acknowledged as disabled instead of falling through and closing the
  component. Outside a component, those global actions keep their normal role.
- `input/config-query`: returns the complete authoritative input map on
  `input/config-state`, correlated by `requestId`.
- Input configuration and component inventory requests keep their large
  scratch state outside the UART/USB receive-task stack. Receive tasks only
  frame bytes on 8 KiB stacks; a dedicated 12 KiB protocol worker serializes
  bounded responses without sharing the display call stack.
- `control/command` with `type: audio_bridge`: enables or disables board-microphone capture.
- `audio/control`: starts or stops capture for diagnostics; `audio/query` returns
  the active capture codec plus ES8311 playback state on `audio/status`.
- `asset/slot-query`, `asset/activate`, `asset/begin`, `asset/chunk`,
  `asset/raw-chunk`, `asset/file`, `asset/commit`,
  `asset/patch-commit`, `asset/stat`, `asset/abort`: transactional appearance
  transfer lifecycle plus exact cached-pack discovery/reactivation. A cancelled
  host transfer must send `asset/abort` so the device discards the incomplete
  inactive-slot staging data. Pet Manager records the transaction, file,
  raw-chunk checksum/retry, device acknowledgement, and terminal result as a
  bounded JSONL log under the current app-data `logs/usb-transfer.jsonl` path.
- `widget/begin`, `widget/chunk`, `widget/commit`: installs a bounded declarative
  `.clawpkg` subset and replies on `widget-install-ack`. The P4 runtime rejects
  unknown grammar, fetchers, readers, unsupported package files, and inputs that
  exceed the advertised limits. Every chunk carries `decodedSize` and an FNV-1a
  `checksum`; the chunk ACK echoes `path` and `index`, so the PC can reject stale
  acknowledgements and retry transient corruption without advancing another file.
  Pet Manager serializes bulk installs, and a stale transfer cannot invalidate the
  currently active staging transaction.
  Successful installs are retained in a 16-slot SPIFFS component catalog. A
  slot keeps two bounded package generations, and the catalog itself keeps two
  checksummed, monotonically sequenced snapshots. An existing `widgetId` is
  written to the package generation not referenced by the current catalog; a
  new id uses the first free slot. Firmware reopens and validates both files
  from Flash before one new catalog snapshot makes the package visible. The
  previous snapshot and package generation remain a boot-time rollback point,
  and startup selects the newest snapshot whose complete package set validates.
  Firmware advertises this behavior as
  `capabilities.componentCatalogGeneration=true`. The active component id is
  part of the same snapshot, so install, delete, and selection never expose a
  mixed widget/buttons/catalog/active-id state. Commit may take several seconds
  when SPIFFS garbage collection is active; firmware yields between writes.
  For the built-in `token-usage` package only, Pet Manager removes its Linux host
  reader/fetcher declarations before transfer. Firmware then updates its four
  declared display variables from the validated bounded stats model; arbitrary
  third-party readers and fetchers remain rejected. Chunk retries are idempotent,
  and a failed integrity check rolls back the partial chunk before retrying.
  A tick may use paired `when` and `then` objects for bounded completion logic:
  `when` names an integer variable and an inclusive `lte` threshold, while `then`
  may select a declared state and apply the same bounded `set`/`inc` effects as a
  transition. Each transition or tick top-level effect group accepts at most four
  combined `set`/`inc` entries; a tick's `then` group has its own four-effect
  limit. State/page changes do not consume an effect slot. This lets timers stop
  or advance phases without arbitrary code.
- `widget/delete`: optional `preserveSave:true` requires advertised
  `widgetDeletePreserveSave=true` (0.7.99+). It retains private device saves under
  their original component ID for same-ID reinstall; packages must be backed up
  separately. It does not export saves or touch PC private records. Omitted/false
  keeps the existing erase-save semantics. Unsupported clients must not assume
  this policy on an older board.
  Removes the exact installed catalog component idempotently and replies
  on `widget-install-ack` with the same `transferId` and `phase: "delete"` only
  after its package and catalog entry are cleared and the screen returns to
  `main`. Deleting the active entry selects the nearest remaining component
  when possible; deleting another entry does not interrupt the active app.
  Firmware that advertises `capabilities.widgetDelete=true` implements this
  contract.
- `widget/list`: requires a bounded `requestId` and replies on
  `widget/inventory` with that same identity. P4 advertises
  `capabilities.widgetInventory=true`, `supportsMultiple=true`, and
  `maxInstalled=32` on 0.7.85+ (16 on earlier builds); the response enumerates the persisted catalog, marks the
  active component, and uses each bounded runtime title as its optional name.
  Kind and version remain `null` because `component.json` is not persisted.
- `miniapp/event`: dispatches a declarative action directly to the active app.
  Eligibility, host freshness and confirmation deadlines use the device's
  monotonic clock; an optional legacy desktop `tsMs` is not used for timing.
- `app/event`: bounded application-host request containing `boardDeviceId`,
  `componentId`, ephemeral `instance`, increasing `sequence`, `action`, `page`,
  and `visible`. The desktop requires the connected board identity, deduplicates
  actions, and invalidates queued actions on exit/switch/reentry. Status polls
  are not permission to record, play, or dispatch desktop work.
  `app/snapshot` replies echo component/instance/sequence and bounded dashboard
  values; an earlier status cannot overwrite a newer action. No automatic action
  retransmission occurs on timeout. Component-host heartbeats bypass the work
  queue so a slow action cannot keep playback alive after exit.
- `miniapp/query`: returns the active bounded view model on `miniapp/state` for
  PC status display and hardware diagnostics. A configured hardware action can
  explicitly proxy `screen.region.tap` or `screen.region.long_press` to the active
  app, so widgets remain operable without reserving SW1 or another physical input.
- `asset/*`: accepts current `p4-h264-v1` Annex-B streams plus optional
  `p4-pcm-wav-v1` cues under `p4/audio/`. Legacy `p4-mjpeg-v1` remains
  readable for compatibility; Linux `videos/*.mp4` or `videos/*.wav` assets
  are rejected explicitly.
- `firmware/begin`, `firmware/chunk`, `firmware/commit`, `firmware/abort`:
  writes a SHA-256-verified ESP-IDF image to the inactive 2.5MiB OTA slot. The
  desktop uses 4092-byte padding-free decoded chunks when schema 5 and the
  advertised 4KiB receiver are both present, otherwise it starts at the legacy
  2046-byte size. A transient Base64 rejection retries the same sequence at the
  current size; three repeated rejections reduce it through 2046, 1020, then
  510 bytes without restarting the transaction, and 32 successful chunks
  restore the next larger size. Schema-5 firmware has an idle-clock race, so
  the desktop starts at 510 bytes, promotes through the larger sizes after
  stable ACK streaks, and automatically rebuilds a transaction if the old
  firmware clears it. Schema-6 firmware starts directly at 4092 bytes. The firmware
  timeout reaper ignores a loop timestamp older than the most recent chunk so
  unsigned subtraction cannot clear a healthy transfer. Before a new update,
  Pet Manager queries
  `firmware/status`; if a disconnected host left an active transaction, it
  aborts that exact reported `transferId` before starting the replacement.
  Every upload is also pinned to its original USB connection generation, so an
  old worker cannot resume onto a newly opened serial connection. Lost begin
  acknowledgements are aborted and retried within the same update action.
  During firmware and appearance writes the renderer uses a lightweight
  transfer-only screen that does not read changing assets, instead of leaving
  the previous frame apparently frozen. After reboot the backlight remains
  hidden through built-in component migration and is revealed only after the
  first complete frame has rendered.
- `firmware/query`: returns running, boot, and next partition metadata on
  `firmware/status`, including the active `transferId`, byte/sequence progress,
  and an echoed optional `requestId` for exact desktop correlation.
- `diagnostics/query`: returns boot/reset history, heap and PSRAM low-water
  marks, SPIFFS usage, task/GPIO/touch/audio health, joystick
  center/current/minimum/maximum ADC samples, current page,
  `sessionQueueCount`, ordered `sessionQueueIds`, `currentSessionId`,
  `retainedSessionCount`, firmware, `buildId`, `gitSha`, `buildDirty`,
  `protocolSchema`, and running partition on
  `diagnostics/status`.
  `memory` additionally separates `freeInternalBytes`, `minimumFreeInternalBytes`,
  `largestInternalBlockBytes`, `freeDmaBytes`, `largestDmaBlockBytes` and
  `largestPsramBlockBytes`: free PSRAM is not interchangeable with internal/DMA RAM.
- `system/reset-inputs`: restores the safe SW1/SW2/SW3/encoder defaults and
  replies on `diagnostics/action`. It does not erase appearance assets.
- `system/reboot`: acknowledges on `diagnostics/action`, then performs a delayed
  software restart so the response can leave the UART first.

Current Session example:

```json
{
  "topic": "session/current",
  "payload": {
    "sessionId": "019f...",
    "title": "修复设备端 Session 名称显示",
    "index": 2,
    "count": 3,
    "sessions": [
      {"id":"019f-a","title":"修复设备端白屏","state":"done","transitionRevision":1740000001000,"terminalRemainingMs":42000},
      {"id":"019f-b","title":"完善会话切换","state":"working","transitionRevision":1740000002000,"terminalRemainingMs":0},
      {"id":"019f-c","title":"验证语音输入","state":"waiting_user","transitionRevision":1740000003000,"terminalRemainingMs":0}
    ],
    "agentId": "codex",
    "activeSessionIds": ["019f-b", "019f-c"],
    "displayEnabled": true,
    "notice": "已切换到下一个会话"
  }
}
```

For the compatibility action IDs `session_previous` and `session_next`, current
firmware resolves the action inside the visible peer page. On `main` it reports
the selected Session metadata so the desktop follows the same card. On
`components` it reports `component_select`, so desktop Session routing does not
change:

```json
{
  "topic": "input/event",
  "payload": {
    "action": "component_select",
    "context": "components",
    "handledLocally": true
  }
}
```

## A/B Firmware Update

The PC serializes firmware updates with other large transfers and binds every
request to the board id that opened the dialog. It parses the ESP-IDF app
descriptor before upload and accepts only `pet_manager_p4_runtime`. It sends
4KB decoded chunks and waits for an ACK whose next sequence and byte count
match that exact chunk. A missing ACK retries the same sequence up to three
times; the board treats the immediately previous sequence and commit as
idempotent retries. The board independently validates total size, chunk
sequence, SHA-256, and project identity before selecting the inactive slot.

```json
{"topic":"firmware/begin","payload":{"transferId":"firmware-42","size":1402704,"sha256":"<64 hex chars>"}}
{"topic":"firmware/chunk","payload":{"transferId":"firmware-42","seq":0,"decodedSize":4096,"data":"<base64>"}}
{"topic":"firmware/commit","payload":{"transferId":"firmware-42"}}
```

Every phase replies with the current byte and sequence counters:

```json
{"topic":"firmware/ack","payload":{"transferId":"firmware-42","phase":"chunk","ok":true,"receivedBytes":4096,"expectedBytes":1402704,"nextSequence":1,"targetPartition":"ota_1"}}
```

After commit, the device reboots into the selected slot. A new image remains
`pending_verify` until repeated LCD render samples stay healthy for 8 seconds;
the desktop reports success only after the same board reconnects with the
requested version, target partition, and `imageState: valid`. If the sustained
health window never completes within 20 seconds, the bootloader rolls back.
An interrupted transfer is aborted after 30 seconds of inactivity and resumes
normal rendering. The board keeps the committed runtime alive for 5 seconds
before restart, leaving enough time for the desktop's 3-second commit timeout
and one idempotent retry to receive the cached commit result.

The A/B application image contains a bounded, compressed bootstrap catalog.
Complete artwork belongs to factory/PC resource packages, not duplicated OTA slots.
`pet_p4_miniapp_sync_builtins` adds missing entries, never downgrades an existing
complete artwork package to a bootstrap, removes retired entries, and restores
the active id (otherwise the first visible entry). The unreleased canonical list
in `factory-config.json` has 28 entries, with `daily-news` fourth and `cloud-hop`
first. User-created packages remain ahead of firmware defaults; later installs
prepend normally. `two-key-pong` and `bloomfrog_companion` remain available.
Migration reorders catalog records without moving slots or saved data. A build-id
and bundle-checksum marker is written only after successful completion; an
interrupted migration retries on the next boot. Component ids outside the firmware-owned list are preserved.
Use `tools/read_component_inventory.py` only with an idle serial port to compare
the actual installed inventory; a bundled count does not prove installation.

Firmware OTA, asset OTA, and delayed reboot are mutually exclusive. Once an
OTA commit or diagnostic reboot is pending, new asset/native transfers are
rejected until the board restarts. Boot validation samples LCD/render and both
transport RX tasks independently of asset rendering, so a long asset transfer
cannot accidentally starve the health window or mark a bad image healthy.

## Hardware Input Configuration

Inputs are sampled every 5ms. Buttons are active-low with internal pull-ups,
25ms debounce and a 700ms long-press threshold. On current hardware the
joystick X/Y axes use ADC1 GPIO21/GPIO20. The runtime calibrates their neutral
center at boot, derives activation/release hysteresis independently from the
available travel in each direction, resolves diagonals by normalized dominant
axis, and repeats a held direction after a bounded delay. This accepts
limited-travel joystick batches without weakening the neutral dead zone. The
legacy EC11 decoder on GPIO2/GPIO3 remains active, so the same firmware also
supports the previous board revision. Firmware queues input events outside
GPIO and render callbacks, and stores accepted mappings in NVS.

PC to board:

```json
{
  "topic": "input/config",
  "payload": {
    "requestId": "input-42",
    "version": 9,
    "bindings": [
      {"event":"button.sw1.short_press","action":"page_enter","value":""},
      {"event":"button.sw1.long_press","action":"disabled","value":""},
      {"event":"button.sw1.hold","action":"voice_ptt","value":""},
      {"event":"button.sw2.short_press","action":"component_center","value":""},
      {"event":"button.sw2.long_press","action":"disabled","value":""},
      {"event":"button.sw2.hold","action":"disabled","value":""},
      {"event":"button.sw3.short_press","action":"page_back","value":""},
      {"event":"button.sw3.long_press","action":"disabled","value":""},
      {"event":"button.sw3.hold","action":"disabled","value":""},
      {"event":"button.encoder.short_press","action":"page_enter","value":""},
      {"event":"button.encoder.long_press","action":"disabled","value":""},
      {"event":"button.encoder.hold","action":"disabled","value":""},
      {"event":"knob.rotate_ccw","action":"session_previous","value":""},
      {"event":"knob.rotate_cw","action":"session_next","value":""},
      {"event":"joystick.up","action":"session_previous","value":""},
      {"event":"joystick.down","action":"session_next","value":""}
    ]
  }
}
```

Allowed actions are `disabled`, `voice_ptt`, `agent_enter`, `agent_prompt`,
`session_next`, `session_previous`, `session_clear`, `miniapp_screen_tap`,
`miniapp_screen_long_press`, `component_center`, `miniapp_action`,
`page_toggle`, `page_enter`, `page_back`, `page_main`, and `page_app`.
The firmware keeps this full set for stored-config and component compatibility.
Pet Manager's P4 button menu exposes only custom prompt, voice input,
previous/next item, clear sessions, pet/component page toggle, confirm, back/cancel,
and unbound.

SW1 short press and joystick center short press both default to `page_enter`:
they activate the selection and open `app` only from `components`; they do not
open the catalog from `main`. SW3 short press defaults to the global
`page_back` path. Center long press defaults to `disabled`; joystick Up/Down
default to `session_previous`/`session_next`. All remain editable and are
persisted with the rest of the input map. SW2 short press toggles `main` and
`components` in both directions. Other SW short/long gestures
default to `disabled`, except SW1 long
press, whose hidden `.hold` transport defaults to `voice_ptt`. Joystick left
and right deliberately retain `knob.rotate_ccw` / `knob.rotate_cw` event names,
so old component packages continue to work without conversion. Inside the
catalog, previous/next selects the prior/following catalog entry. On `main`, the
same actions select Session bubbles. Neither action changes the two-dot peer
page; SW2 does that. While `app` is open, component bindings—including SW2—retain priority.
New packages may bind `joystick.up` and `joystick.down`.

`page_toggle` switches between `main` and `components`; from `app` it returns to
`components`. `page_main` and `page_app` remain accepted for older persisted
device configs. Component packages no longer own navigation: PC downlink removes
legacy `page_main/page_back` records, and firmware ignores any such records that
remain in an already-installed package. While `app` is open, whichever persisted
global event currently maps to `page_back` is resolved before component gameplay
bindings, so changing the global exit key immediately changes every component.
The default remains SW3 short press; SW1 long-press PTT is unaffected. The two
legacy-named session actions now select content within the current peer page. The two mini-app proxy actions only run while
the app page is open and dispatch its existing `screen.region.tap` or
`screen.region.long_press` binding. Custom values are limited to 159 UTF-8 bytes.
The board replies on `input/config-ack`; legacy config uses `button-config-ack`.
The desktop reads the NVS-backed device map after opening the dashboard or
reconnecting USB. This read waits for any firmware or asset transaction to
finish before using the serial link:

```json
{"topic":"input/config-query","payload":{"requestId":"device-request-42"}}
```

The board correlates the response and returns the complete persisted map. The
desktop compares it with the complete PC mapping; any difference is resolved by
writing the PC mapping to the board. The board response never replaces the PC
cache, and a failed read falls back to the same full PC write:

```json
{
  "topic": "input/config-state",
  "payload": {
    "requestId": "device-request-42",
    "ok": true,
    "boardDeviceId": "p4-a1b2c3d4e5f6",
    "runtime": "esp-p4",
    "bindingCount": 16,
    "config": {
      "version": 7,
      "voiceEnabled": false,
      "voiceButton": "",
      "bindings": [
        {"event":"button.sw1.short_press","action":"page_back","value":""}
      ]
    }
  }
}
```

Pet Manager places the P4 device illustration above four matching control
groups and exposes twelve logical gestures: short/long press for SW1, SW2, and
SW3, plus joystick center short/long press and four directions. Left/right keep
the counter-clockwise/clockwise compatibility event names. The internal `.hold`
rows are never shown separately.
A visible long-press row is encoded as two mutually exclusive board bindings:
`.long_press` for one-shot actions and `.hold` for `voice_ptt` start/end.
SW1 long press defaults to `voice_ptt`; the shared switch in Button
Configuration and Voice Assistant decides whether its `.hold` binding is
`voice_ptt` or `disabled`. Turning voice off retains the visible client
selection but sends both SW1 long-press bindings as `disabled`. The complete
ten-row client model therefore remains a fourteen-binding board map. Config
version 3 migrates version-2 values that exactly match either previous default
layout to this map while preserving unrelated custom actions.
While `app` is open, the persisted global `page_back` event is resolved first.
Other events declared in the component's `buttons.json` then resolve as gameplay
or tool actions; outside `app`, or for an undeclared component event, the
persisted device-page mapping remains authoritative. Package-authored system
navigation actions are ignored. A component-declared `.long_press` also
suppresses the matching global `.hold` while that component is open, preventing
an editable PTT binding from firing underneath the component action.

Board to PC:

```json
{
  "topic": "input/event",
  "payload": {
    "version": 3,
    "seq": 17,
    "boardDeviceId": "p4-a1b2c3d4e5f6",
    "control": "key.1",
    "gesture": "hold_start",
    "event": "button.sw1.hold",
    "context": "main",
    "action": "voice_ptt",
    "handledLocally": false,
    "dropped": 0
  }
}
```

## Touchscreen Gestures

GT911 is sampled every 15ms without driving reset or interrupt GPIOs. The local
board profile declares separate LCD/touch reset pins; that is not proof that
the installed panel routes both pins. Startup probes both legal addresses up
to four times, with bounded delays, without resetting the shared audio I2C bus.
Physical portrait coordinates are converted into the 640x480 logical UI.
For the installed CST826 panel, physical testing showed the sensor axes are
180 degrees from the LCD native axes: its board-specific mapping is
`uiX=639-rawY, uiY=rawX`. GT911 / FT6336U keep the existing mapping. This is
panel orientation, not a universal CST826 property. The diagnostic page shows
a persistent marker while pressed; health reports raw/mapped coordinates,
queued-pointer age and stale-event counts to separate mapping from latency.
If both GT911 addresses are absent, the product probes `0x15` and `0x38`.
CST826 requires chip type `0x11` at register `0xAA`; FT6336U requires chip ID
`0x64` at register `0xA3`. These enable the bounded six-byte-point report reader;
an unknown responding device is rejected. This path reads touch reports only,
without changing controller tuning registers. The register format and ID follow
[Espressif's FT6336U driver](https://github.com/espressif/esp-iot-solution/blob/master/components/display/lcd_touch/esp_lcd_touch_ft6336u/esp_lcd_touch_ft6336u.c).
The CST826 identity and report layout are documented in
[Adafruit's CST8XX driver](https://github.com/adafruit/Adafruit_CST8XX_Library).
Register `0xA3` returning zero alone does not rule out CST826.
`touchInput.controller` reports the selected controller, not a hardcoded GT911.
Tap and long press route to `screen.region.tap` and
`screen.region.long_press`; four-direction swipes move from main to the component
center, from the component center to main, or from an app to the component center.
Realtime conversation owns its history scrolling. Main-page tap feedback selects a bounded `touch.*` family but cannot
interrupt `waiting_user` or `error`.

Each recognized gesture is emitted on `input/event` with control
`screen.touch`, logical `x`/`y`, `dx`/`dy`, duration, local handling status,
and the cumulative dropped-event count. Diagnostics separately reports
`touchReady` and `touchDroppedEvents`. `runtime.touchHealth` includes initialization
stage/error, selected address, both probe results, sample/contact counts and poll
error counters. Three consecutive read failures report touch unavailable until
a successful poll. Read errors, ambiguous/multiple contacts and queue overflow
cancel a stroke rather than synthesizing a swipe or connecting stale points.

### Native touch diagnostic (not a PetUI package capability)

`diagnostics/touch-test` with `{"action":"start"}` enters a temporary single-fruit
slicing test only when touch is ready and no conversation/transfer is active.
`{"action":"stop"}` returns home. Replies use `diagnostics/action`; the test
expires after 120 seconds. SW1 restarts, with configured global Back/realtime
bindings taking priority. A captured stroke never becomes global swipe navigation.
Points stay local (no per-sample USB traffic); score appears in touch diagnostics.
The native prototype uses swept hit testing, bounded trails and three-miss
settlement. This diagnostic is independent of installed components.

### PetUI touch slicing

`capabilities.widgetScenes` now includes `p4-touch-slice-v1`, with
`widgetPointer: "p4-pointer-v1"`. Installation requires `touchInput.ready=true`;
an old firmware or disconnected touch controller must not be treated as ready.
`widgetSliceRevision: 2` adds three selectable modes, radius-based collision,
golden fruit and bounded double-score energy. A package declaring
`scene.revision: 2` must not install onto revision-1 devices.
Top-level `p4-bounded-runtime-v4` packages can select this bounded scene and
provide four PNG fruit sprites, a 30–120 second duration, 1–5 lives and a
0–100 effect volume. Package scripts and executable assets remain forbidden.
The strict schema and original component generation workflow are documented in
`../pc/skills/petui/references/touch-slice.md`.

The native core owns fixed-size fruit, particle and trail pools, continuous
segment collision, combo scoring, bomb priority and settlement. Touch remains
local rather than streaming through USB. Global exit, conversation and transfer
screens suspend play; resuming requires the pause action. Sounds share the
existing audio task and stop on pause/exit. The PC preview and skill smoke test
run the same C core compiled into a bundled, import-free, 128 KiB WASM module;
no code is loaded from component packages.

For hold bindings, `hold_start` is emitted only after the 700ms threshold;
`hold_end` is emitted on release. Page actions set `handledLocally: true`.

## USB PTT Audio

The default `button.sw1.hold -> voice_ptt` mapping starts board-microphone capture on
`hold_start` and stops it on `hold_end`. Audio is PCM S16LE, 16kHz, mono, in
20ms (640-byte) frames. Capture stops automatically after 30 seconds.

Board to PC stream:

```json
{"topic":"audio/begin","payload":{"sessionId":"p4-audio-123-1","boardDeviceId":"p4-...","sessionQueueEmpty":true,"format":"pcm_s16le","sampleRate":16000,"channels":1,"bitsPerSample":16,"frameMs":20,"transport":"usb-jsonl-pcm-v1"}}
{"topic":"audio/chunk","payload":{"sessionId":"p4-audio-123-1","seq":0,"bytes":640,"checksum":"0123456789abcdef","data":"<base64>"}}
{"topic":"audio/end","payload":{"sessionId":"p4-audio-123-1","reason":"released","chunks":50,"bytes":32000,"durationMs":1000,"checksum":"0123456789abcdef"}}
```

`sessionQueueEmpty` freezes whether the device had zero visible cards at the
instant capture started. When true, the PC still resolves the target from its
independent P4 Session binding; Codex and Claude use their visible composer path
to foreground that Agent's current bound Session before live text updates and
submission. Older firmware that omits the field is treated as `false`.

Each chunk checksum is FNV-1a 64-bit over the decoded frame. The final checksum
is FNV-1a over the complete PCM stream. The PC rejects session, sequence, byte
count, or checksum mismatches. P4 PCM is retained for bounded Windows zh-CN
recognition without opening the PC microphone; legacy boards may still use the
local voice-service relay at `127.0.0.1:50001`. `audio/error` terminates the
current session. `audioCapture.codec` reports `ES7210`, `ES8311`, or
`unavailable` according to the initialized capture path.

## Realtime Conversation Audio

Firmware 0.7.57 implements a realtime persona conversation mode on top of the PTT
stream. The PC owns VAD, ASR, the persona LLM and TTS; the board only captures,
plays, and shows a HUD.

PC to board:

```json
{"topic":"audio/conversation","payload":{"enabled":true,"halfDuplex":true,"sessionId":"rtc-1"}}
{"topic":"audio/play_begin","payload":{"sessionId":"rtc-1-turn","sampleRate":16000,"channels":1,"format":"pcm_s16le"}}
{"topic":"audio/play_chunk","payload":{"sessionId":"rtc-1-turn","seq":0,"bytes":3200,"data":"<base64 PCM S16LE 16 kHz mono>"}}
{"topic":"audio/play_end","payload":{"sessionId":"rtc-1-turn","bytes":64000}}
{"topic":"audio/play_flush","payload":{"sessionId":"rtc-1"}}
{"topic":"ui/conversation","payload":{"sessionId":"rtc-1","state":"listening","name":"小西","text":""}}
```

- `audio/conversation` with `enabled:true` starts continuous microphone capture
  (same `audio/begin` / `audio/chunk` / `audio/end` framing, `audio/begin`
  carries `"conversation":true`, no 30 s cap). With `halfDuplex:true` the board
  keeps reading the microphone but drops frames while the speaker is playing and
  for 250 ms afterwards. `enabled:false` stops capture, flushes playback and
  clears the HUD.
- `audio/play_begin` opens a streamed playback turn; `audio/play_chunk` carries
  up to 4096 decoded bytes per line into a 3 s ring buffer; playback starts once
  200 ms are buffered (or immediately on `play_end`), and `play_flush` drops
  everything for barge-in / session end. State WAV cues yield while a stream is
  active. `audio/status` gains `conversation`, `streamActive` and
  `streamBufferedMs`.
- `audio/diagnostic` reports metadata-only lifecycle/error events (event, sessionId,
  sequence, bytes, bufferedMs, uptimeMs, ok). The PC persists these alongside its
  own realtime diagnostics; no PCM or dialogue text is logged. `audio/status.playbackVolume`
  reports the codec volume readback (100 / 0 dB in 0.7.57, shared by v1 and v3). `audio/status`
  adds `streamUnderruns` and `streamReceivedBytes`.
- `capabilities.features.conversationSubtitles:true` advertises synchronized captions.
  `ui/conversation` accepts `sessionId`, `state`, `name`, `text`, `role` (`user` or
  `pet`) and `final` (boolean). It owns the main-page bubble while active; Agent
  session cards and statistics remain hidden but continue updating in the background.
  The existing card style uses a blue outline. Listening displays live ASR text;
  replies display only the pet's current sentence, never a retained user summary.
  `state:"error"` displays `text` as a failure hint for 8 seconds. The animation
  family follows the state — `listening`
  → `waiting_user`, `thinking` → `working`, `speaking` → `welcome`. `state:"ended"`
  clears it. Host disconnection also clears captions and restores Agent display.
- `audio/play_subtitle` carries `{sessionId,offsetBytes,text}` after `audio/play_begin`
  and before its sentence's PCM. The ID must match the playback turn, and the offset
  is an even unsigned byte count from that turn's beginning (16 kHz mono S16LE).
  The device advances captions using consumed PCM, not elapsed wall time. Up to 12
  pending cues and 8 finalized captions of at most 511 UTF-8 bytes are held in RAM;
  overflow/invalid cues are rejected. `audio/play_flush` discards pending cues.
  Joystick/touch navigation only reviews this bounded history; new captions restore
  the live view. Exiting clears all caption/history text. No dialogue text is logged.
- The `realtime_chat` input action is emitted as a normal `input/event`; the PC
  starts conversations only from `main` (the pet screen). While active, it consumes
  all input from that board; the realtime shortcut or configured back action ends
  the conversation, and other keys cannot submit text or change Agent sessions.

## Protocol ACK/NACK

While USB serial is connected, Pet Manager sends `system/heartbeat` every two
seconds. The board considers the desktop disconnected after six seconds without
any valid host message. This drives the on-device waiting, initialization, and
disconnected status banner (`客户端未连接`); normal pet playback remains
unobstructed once the host is bound and an appearance pack is available.

Supported non-streaming messages that include `payload.requestId` receive a
`protocol/ack` success response. Malformed, unknown, and recognized-but-not-yet-
implemented topics receive an immediate NACK instead of being ignored:

```json
{
  "topic": "protocol/ack",
  "payload": {
    "requestTopic": "control/apply-wifi",
    "requestId": "req-42",
    "ok": false,
    "code": "unsupported_topic",
    "error": "topic is not supported by this firmware"
  }
}
```

Unrecognized topics use `code: "unknown_topic"`; malformed envelopes use
`invalid_json` or `invalid_message`. Asset transfers continue to use the more
specific `asset/ack` contract.

## Speech Payload Shape

The PC can reuse the legacy Linux runtime speech card fields:

```json
{
  "topic": "speech/text",
  "payload": {
    "displayTitle": "pet dev",
    "displayContent": "Building the P4 runtime...",
    "status": "working",
    "statusText": "thinking",
    "tsMs": 1780000000000
  }
}
```

The firmware also accepts `sessionName`/`title` for the title and `text`/`message`
for the body, so older bridge payloads still render.

## Statistics Payload Shape

The PC forwards the active Agent state with token usage and bounded runtime
metrics. When `dailyTokenUsage` is available, it also sends that
current-followed-Agent daily aggregate on `runtime/stats`, independently of the
device bubble's selected Session. The regular state compactor continues to
prefer that aggregate over a single Session's `tokenUsage` for compatibility.
The P4 parses the normalized fields when the message arrives, then renders from
a fixed-size model without retaining an unbounded JSON tree, and replays the
model whenever the Token mini-app is activated. Changing the followed source
clears stale stats before the new source is applied.

```json
{
  "topic": "stats/update",
  "payload": {
    "source": "codex",
    "state": "working",
    "sessionTitle": "pet dev",
    "tokenUsage": {
      "totalTokens": 18432,
      "inputTokens": 13000,
      "outputTokens": 2432,
      "cachedInputTokens": 3000,
      "estimatedCostUsd": 0.0421,
      "modelContextWindow": 128000
    },
    "metrics": {
      "latency": {"turnMs": 12340, "firstTokenMs": 820},
      "toolCalls": 4,
      "toolErrors": 0,
      "waitingUserMs": 0,
      "contextUsagePct": 14.4
    },
    "tsMs": 1780000000000
  }
}
```

## Asset Direction

The Linux runtime consumes MP4/WAV. The P4 runtime consumes preprocessed packs:

- `p4/manifest.json` with format `p4-h264-v1`.
- `p4/families/sha256-<digest>.h264` H.264 Annex-B streams.
- Each family is aspect-fitted into a 640x480 canvas with black letterboxing and at most
  225 frames. Clips up to 15 seconds retain the 15 fps ceiling; longer clips
  lower the sampling rate so those frames span the complete source timeline.
- Every family records `fps` for older-firmware fallback and exact
  `durationMs` timing. Firmware selects frames proportionally across that exact
  duration; `frameDurationMs` remains available as an older-firmware fallback.
- FFmpeg exports yuv420p Baseline H.264 at CRF 27 with no B-frames. The desktop
  and packaging prebuilder then replace x264's SPS with the minimal SPS proven
  against `esp_h264`; device synchronization only validates and transfers the
  finished ready pack and never invokes FFmpeg.
- Current macOS-bundled `0.7.50-p4` USB-UART firmware runs UART0 at 4 Mbaud. It receives a small
  `asset/raw-chunk` JSON header followed immediately by bounded binary data
  and verifies the per-chunk FNV-1a checksum in RAM. The protocol accepts up
  to 65,536 bytes; the desktop deliberately uses 8,192-byte chunks for the
  CH343 link. Every ACK echoes the exact chunk index. A timeout flushes the
  pending binary frame and retries the same index, while firmware recognizes
  matching duplicate chunks without writing them to flash a second time.
  H.264 targeting slot 1 is written sequentially to the dedicated `appearance`
  partition; manifest/WAV files remain in SPIFFS. A 4KiB lookup header is
  written only at commit, before the ready marker and active-slot switch.
  This removes Base64 expansion, JSON bulk parsing, and SPIFFS video-write
  amplification. The 64KiB value is a device maximum, not a required host
  chunk size: macOS uses 8KiB logical chunks with drained 64-byte writes,
  while Windows `0.7.50-p4` retains its 2 Mbaud transport, starts at up to 64KiB, and
  restarts the aborted transaction at 2KiB after a checksum or timeout failure.
  Windows CH343 raw
  bodies use paced 128-byte slices while control frames retain 64-byte writes.
  Raw-partition erase is performed
  in 16KiB yielding slices so the UART receive task cannot trip its watchdog.
  Windows probes 2 Mbaud first; macOS probes 4 Mbaud first. Both retain
  3 Mbaud, the other high-speed rate, 921,600, and 115,200 compatibility fallbacks.
- Older firmware without `transport.rawAssetChunks` continues to receive
  20,478-byte pre-Base64 `asset/chunk` JSON Lines. File checksum commits remain
  authoritative for both transports.
- Native USB vendor bulk can query and activate an exact pack already cached
  on the device. Until native USB reuses the dedicated raw-partition writer, a
  cache-miss pack containing H.264/MJPEG is rejected by the desktop before the
  first full-sync write; USB-UART is the supported cache-miss path. Every
  native mutation is bound to the requested `boardDeviceId` through the
  challenge-response handshake above, including when several matching
  `303A:4040` devices are connected.

Example manifest:

```json
{
  "format": "p4-h264-v1",
  "packId": "<sha256-of-sorted-payload-paths-and-bytes>",
  "codec": "h264",
  "container": "annex-b",
  "width": 640,
  "height": 480,
  "fps": 15,
  "families": [
    {
      "family": "idle.default",
      "path": "p4/families/sha256-<digest>.h264",
      "frames": 225,
      "streamBytes": 738259,
      "fps": 15,
      "frameDurationMs": 68,
      "durationMs": 15130
    }
  ]
}
```

The PC must not treat MP4/WAV OTA as successful on P4. The board accepts only
`p4/...` logical paths and returns explicit error acks for Linux asset paths.

The manifest is parsed into a fixed-capacity catalog only when an asset slot is
loaded or committed. Runtime selection plays `welcome` once, rotates available
`idle.*` and `working*` families only after each selected clip reaches its own
full duration, maps `waiting_user`, `done`, and `error` directly, and prefers
`working.browsing` while speaking when that family exists. Every family switch
starts playback at frame zero; touch and welcome reactions are not clipped by a
fixed five-second lifecycle limit. When Session cards are present, this family
selection uses the current card's lifecycle rather than the aggregate Agent state.
Selected content-addressed H.264 streams are loaded into bounded PSRAM cache
slots and decoded sequentially. A timeline loop or family change reopens the
decoder at frame zero, so lifecycle changes interrupt the old action on the
next render cycle while uninterrupted actions retain their real duration.
YUV conversion always targets the logical 640x480 RGB565 framebuffer before a
separate 270-degree rotation. The former combined direct-native conversion is
not used. Two PSRAM native-output buffers alternate between frames, preventing
PPA writes for the next frame from mutating memory still being copied by the
LCD DMA path after a delayed refresh.
Session-card overlays are likewise cached by queue content and selected index,
with only animated working markers redrawn per frame.
# 公共状态提示音（0.7.63+）

设备能力 `appearance.systemCues: true` 表示 manifest 可声明 `systemCues: true` 且不携带默认 WAV；该位只是 manifest 契约，保持为 `true` 以兼容桌面端校验。**固件不再内置默认完成、错误、等待用户音效**（为 WiFi 协议栈让出约 240 KB 的 OTA 空间）：没有专属音效的形象保持静音。`families[].audioPath` 配合 `audioSource: "custom"` 的专属音效照常播放；已知旧默认 WAV 仍按内容指纹识别并跳过（不再回退到公共音效），不按文件名猜测用户音效。

内置形象切换使用现有 `asset/slot-query` 与 `asset/activate`：仅对设备声明 `builtinProtected` 的内置形象请求，选择有效 slot 0，并使用设备返回的 packId 激活；不要求与 PC 内置 packId 相等。槽位无效时停止并报错，不能回退成写入自定义槽位。普通自定义包仍严格校验 packId。
# 原生敲击组件能力（0.7.70-p4）

设备通过 `widgetInstrument: p4-instrument-v1` 声明本地敲击音源支持。受控组件使用 `instrument.source = percussion.wooden-fish`，`instrument.strike` 只由绑定按键触发；`effect_volume` 和 `ambience_volume` 为 0–100 的整数。缺少能力声明时 PC 阻止安装并提示升级，不模拟成功。组件退出淡出，录音、实时对话和媒体流优先；音频不需要新增 USB 流协议。
# PC 扩展媒体布局（p4-media-surface-v1）

设备声明 `widgetMediaNotice:true` 时，支持 `media/notice`：`{source,state,message,requestId}`。
state 只接受 idle/buffering/error，message 最多 240 UTF-8 字节；source 必须匹配当前媒体视图。
仅在媒体未播放时更新提示，不改变音频所有权、播放列表或音量，也不续期媒体看门狗。
回复沿用 media/status；不匹配 source 或非法字段拒绝，正在播放时忽略通知。

`media.source=pc.<component-id>`、`mode=live` 随 media/library、media/begin 和 media/event 标识所有者。设备通用 surface 由包声明 panel/text/meter/ticks/list/transport，最多 20 个节点、640×480、RGB565、无脚本。`media.sync` 通过列表操作请求 PC 同步；歌词与 seek 不适用于 live。全局停止事件带 `global:true`，PC 取消所有扩展的播放/准备，不取消实时语音。PC 配置不下发；契约详见 `pc/skills/petui/references/pc-extensions.md`。
# 媒体绘制性能说明（0.7.81）

`p4-surface-images-v1` 的压缩格式、配额和事务协议不变。渲染器以 PSRAM 缓存首个静态全屏背景，在图片修订改变时失效；颜色预计算与 Q16 增量旋转避免逐像素浮点除法。带旋转节点的声明式媒体界面请求 33 ms 刷新，但实际帧率由设备日志验收，不作为网络音频无缓冲的承诺。
# PetUI 既有游戏精细画面（0.7.88）

能力 `widgetGameSurface=p4-game-surface-v1` 允许维护已有 snake/blocks/flappy
包时设置有界 `surface.game_view`。布局、图片、字体仍走 App Surface，
玩法仍走原有 C 核心。无此能力的设备拒绝安装，不静默丢掉画面。
详细契约见 `pc/docs/classic-game-surface.md`。

高清内置资源随 PC/出厂资源事务安装，不占两份 OTA 应用空间。
固件 zlib 启动包解压上限 128 KiB，原生图片启动布局仅补缺失项，
不能覆盖已安装的完整资源包。压缩数据摘要参与升级标记。

# 0.7.91 可选组件能力

- `widgetLifecycle=p4-lifecycle-v1`：原生/场景游戏声明暂停动作与独立最高分变量。
  隐藏时冻结逻辑时钟，恢复不补帧；最高分通过组件自己的存档保存。
- `widgetMediaTouch=p4-media-touch-v1`：媒体 surface 显式声明 input 后，
  可用现有七个受控 media 动作作为节点触屏目标，不接受任意指令。
- `widgetSurfaceAtlas=p4-wide-atlas-v1`：普通图集最大 512×240，宽图使用 P4I2
  的 16 位宽高；仍最多四张图、98,304 个调色板像素和 128 KiB 编译资源。
  字形图集保留 240×240 上限，P4I1/P4J1 与旧组件继续兼容。

安装端必须单独检查这些 capability。声明支持与实机已验收是两回事；
离线测试的能力 JSON 不可替代实际设备握手。
## 0.7.92 开发中的受控输入与字格能力

握手新增 `widgetInput=p4-input-v1` 与 `widgetGlyphCells=p4-glyph-cells-v1`，
安装前由 PC 校验实际握手；离线测试 fixture 不能替代握手。
输入策略详见仓库 `pc/skills/petui/references/input-policy.md`。
宿主应用快照可带 `ui:{mode,actions,context}`，最多 16 个已声明动作、64 个十六进制
上下文字节。`app/event` 的 confirmed/context 只说明设备确认了当时内容；
PC 仍须核对记录版本、设备/实例/轮次和操作状态，不自动批准其他动作。
禁用、等待和超时不产生重试；10 秒没有有效快照禁用宿主动作。
字体可声明 cell，度量仍为 32/em；旧字格解释与 palette/JPEG 总预算不变。
本版本的场景拖动仍是受控网格坐标，不声明连续物理或完整场景存档能力。

## 0.7.101 附加性能诊断（向后兼容）

`audio/status`、`audio/diagnostic` 等会话元数据增加数值字段 `internalFreeBytes`、`internalLargestBytes`、`captureTxLastUs`、`captureTxMaxUs`、`captureStackMinBytes`。发送耗时是同步串口发送耗时，不是 DSP 耗时；最大值在本轮采集开始时清零。不得将这些指标误认为端到端语音延迟或回声消除质量。

diagnostics 的 `runtime` 增加 `renderLastUs`、`renderP95Us`、`renderMaxUs`、`renderDecodeUs`、`renderWindowSamples`、`renderSubmittedFrames`、`renderUnchangedFrames`。P95/max 取最近完成的 100 帧渲染窗口；窗口尚未完成时 samples 为 0，不能解释为零耗时。相同帧省略 LCD 提交不省略游戏逻辑、输入或动画步进。

## 0.7.103-p4-r1 资源完整性修复

本修复基于 PC 内置 0.7.103，不包含 0.7.108 实验分支。握手增加
`widgetResourceIntegrity: p4-resource-integrity-v1`。`widget/inventory.items`
增加 `resourceState`（ready/bootstrap/damaged/unchecked）和 `imageCount`。
只有 ready 报 `manifestState: valid`；已知无图启动包不再冒充完整资源。
目录文件存在但均无法解析时返回 `recoveryBlocked: true, complete: false`，
禁止自动补装、事务提交和清空旧资源；必须保留现有文件用于显式恢复。

启动保留最新结构有效目录中的全部组件身份、顺序及正常资源；只为损坏项寻找
上一份已提交目录中同 ID、同槽位、校验有效的完整包。不能把未提交的残留图片
或其他组件的备用代次用于恢复。普通切换仅保存当前组件 ID，不覆盖备用目录。

`widget/begin` 可带 `repairResources: true`，仅修复已安装且被识别为 bootstrap
的同 ID 组件；damaged 完整包只接受与已提交目录的界面及图片校验值完全一致的恢复源，
不自动降级或升级。须提供完整图片包，固件读取并校验原有按键配置而不覆盖，保留存档。
begin/commit 遇到录音、对话或播放则拒绝；客户端每次连接仅尝试一次，不循环重试。
修复其他组件不切换当前页面。已存在完整图片资源时拒绝已知无图启动包覆盖。
无有效备用包时设备明确提示需要恢复，完整美术仍来自 PC/出厂资源区，不占应用 OTA 两份空间。

