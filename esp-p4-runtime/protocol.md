# ESP-P4 USB Protocol

The P4 runtime keeps one JSON object per line over the ESP32-P4 USB-UART path
for flashing, rescue, and early logs. When the board Type-C jumper is switched
to the ESP32-P4 native USB OTG path, runtime data should use the high-speed
vendor bulk protocol `pet-usb-native-v1` instead.

JSON messages still use the same outer shape as the current Pet Manager USB
transport:

```json
{"topic":"<topic>","payload":{}}
```

## Handshake

Board to PC:

```json
{
  "topic": "hello",
  "payload": {
    "boardDeviceId": "p4-a1b2c3d4e5f6",
    "runtime": "esp-p4",
    "deviceModel": "ESP32-P4 RISC-V Dual-Core + ESP32-C6",
    "fw": "0.7.37-p4",
    "buildId": "0.7.37-p4+290f402abcd1",
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
      "widgetRuntime": "p4-bounded-runtime-v3",
      "widgetScene": "p4-grid-scene-v1",
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
    "buildId": "0.7.37-p4+0123456789ab"
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
  Firmware additionally removes active cards when no `session/current` snapshot
  arrives for 12 seconds; already terminal cards keep only their original
  remaining 60-second deadline. This protects the screen from stale working
  cards if the desktop bridge stops publishing while other USB traffic remains.
  Encoder previous/next actions rotate immediately across this exact visible
  queue, including retained cards. The resulting `input/event` includes
  `sessionId`, `sessionTitle`, `sessionIndex`, and `sessionCount`, allowing the
  desktop to route and locate the same card without reconstructing a different
  active-only index. A following `session/current` refresh preserves a selected
  retained card by its ID.
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
  runtime metrics are also extracted from `state/<agent>` payloads.
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
- `widget/delete`: removes the exact installed catalog component idempotently and replies
  on `widget-install-ack` with the same `transferId` and `phase: "delete"` only
  after its package and catalog entry are cleared and the screen returns to
  `main`. Deleting the active entry selects the nearest remaining component
  when possible; deleting another entry does not interrupt the active app.
  Firmware that advertises `capabilities.widgetDelete=true` implements this
  contract.
- `widget/list`: requires a bounded `requestId` and replies on
  `widget/inventory` with that same identity. P4 advertises
  `capabilities.widgetInventory=true`, `supportsMultiple=true`, and
  `maxInstalled=16`; the response enumerates the persisted catalog, marks the
  active component, and uses each bounded runtime title as its optional name.
  Kind and version remain `null` because `component.json` is not persisted.
- `miniapp/event`: dispatches a declarative action directly to the active app.
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
  the desktop starts and remains on the 510-byte rescue path while installing
  schema 6; schema-6 firmware restores the 4092-byte fast path. The firmware
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

For `session_previous` and `session_next`, current firmware sets
`handledLocally: true` and adds the exact visible selection:

```json
{
  "topic": "input/event",
  "payload": {
    "action": "session_next",
    "sessionId": "019f...",
    "sessionTitle": "完善会话切换",
    "sessionIndex": 2,
    "sessionCount": 3,
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

The A/B application image contains the canonical runtime/button JSON for all
seven built-in components. On first boot of each clean firmware build,
`pet_p4_miniapp_sync_builtins` compares compacted package checksums with the
existing A/B component catalog, replaces changed same-id built-ins, adds missing
ones if slots are available, removes retired `falling-catch`, and restores the
previous active component (or defaults to `two-key-pong`). A build-id marker is
written only after the migration completes, so an interrupted migration retries
on the next boot. Component ids outside the firmware-owned list are preserved.

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
    "version": 7,
    "bindings": [
      {"event":"button.sw1.short_press","action":"page_back","value":""},
      {"event":"button.sw1.long_press","action":"disabled","value":""},
      {"event":"button.sw1.hold","action":"voice_ptt","value":""},
      {"event":"button.sw2.short_press","action":"component_center","value":""},
      {"event":"button.sw2.long_press","action":"disabled","value":""},
      {"event":"button.sw2.hold","action":"disabled","value":""},
      {"event":"button.sw3.short_press","action":"page_enter","value":""},
      {"event":"button.sw3.long_press","action":"disabled","value":""},
      {"event":"button.sw3.hold","action":"disabled","value":""},
      {"event":"button.encoder.short_press","action":"page_enter","value":""},
      {"event":"button.encoder.long_press","action":"disabled","value":""},
      {"event":"button.encoder.hold","action":"disabled","value":""},
      {"event":"knob.rotate_ccw","action":"session_previous","value":""},
      {"event":"knob.rotate_cw","action":"session_next","value":""},
      {"event":"joystick.up","action":"disabled","value":""},
      {"event":"joystick.down","action":"disabled","value":""}
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
previous/next session, clear sessions, component center, confirm, back/cancel,
and unbound.

SW3 short press and joystick center short press both default to `page_enter`:
from `main` either one opens `components`, and from `components` it activates
the selection and opens `app`. SW1 short press defaults to the global
`page_back` path. Center long press and the new up/down directions default to
`disabled`; all remain editable and are persisted with the rest of the input
map. SW2 short press opens the component center. Other SW short/long gestures
default to `disabled`, except SW1 long
press, whose hidden `.hold` transport defaults to `voice_ptt`. Joystick left
and right deliberately retain `knob.rotate_ccw` / `knob.rotate_cw` event names,
so old component packages continue to work without conversion. Inside the
catalog, left/up selects the previous entry and right/down selects the next;
outside the catalog, left/right retain their previous/next-session defaults.
New packages may bind `joystick.up` and `joystick.down`.

`page_toggle` switches between `main` and
the active `app`; `page_main` and `page_app` remain accepted for older persisted
device configs. Component packages no longer own navigation: PC downlink removes
legacy `page_main/page_back` records, and firmware ignores any such records that
remain in an already-installed package. While `app` is open, whichever persisted
global event currently maps to `page_back` is resolved before component gameplay
bindings, so changing the global exit key immediately changes every component.
The default remains SW1 short press; SW1 long-press PTT is unaffected. The two session
actions move through the current live conversation queue; they do not persist
or configure a fixed Session id. The two mini-app proxy actions only run while
the app page is open and dispatch its existing `screen.region.tap` or
`screen.region.long_press` binding. Custom values are limited to 159 UTF-8 bytes.
The board replies on `input/config-ack`; legacy config uses `button-config-ack`.
The desktop reads the authoritative NVS-backed map after opening the device
dashboard or reconnecting USB:

```json
{"topic":"input/config-query","payload":{"requestId":"device-request-42"}}
```

The board correlates the response and returns the complete persisted map. The
desktop updates its in-memory model and local cache only after this response:

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

GT911 is sampled every 15ms without toggling the reset GPIO shared with the
LCD. Physical portrait coordinates are converted into the 640x480 logical UI.
Tap and long press route to `screen.region.tap` and
`screen.region.long_press`; any four-direction swipe changes between main and
the installed app page, or remains on main when no app is installed. Main-page tap feedback selects a bounded `touch.*` family but cannot
interrupt `waiting_user` or `error`.

Each recognized gesture is emitted on `input/event` with control
`screen.touch`, logical `x`/`y`, `dx`/`dy`, duration, local handling status,
and the cumulative dropped-event count. Diagnostics separately reports
`touchReady` and `touchDroppedEvents`.

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
metrics. The P4 parses these fields when the message arrives, then renders from
a fixed-size model without retaining an unbounded JSON tree.

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
- Current USB-UART firmware runs UART0 at 4 Mbaud. It receives a small
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
  amplification. Raw-partition erase is performed in 16KiB yielding slices so
  the UART receive task cannot trip its watchdog. The PC probes 4 Mbaud first
  and retains 3 Mbaud, 921,600, and 115,200 compatibility fallbacks.
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
fixed five-second lifecycle limit.
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
