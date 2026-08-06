# Pet Manager ESP32-P4 Runtime

`esp-p4-runtime` 是 Pet Manager 的 ESP32-P4 设备端固件。它和 `ref/` 桌面端配合：
PC 端负责 agent 监听、素材预处理和大文件传输，P4 端负责 USB 协议、状态接收、
LCD 渲染、形象播放和调试截图。

这个 runtime 和 `legacy/board-runtime/` 是两条不同路径：

- `esp-p4-runtime/`：当前 P4 主线，ESP-IDF 固件，面向 MCU、小屏和 USB 直连。
- `legacy/board-runtime/`：legacy Linux/Raspberry Pi runtime，包含 systemd、ffmpeg、
  framebuffer、MQTT 和完整 widget 运行时。

## Target Hardware

- SoC: ESP32-P4 RISC-V Dual-Core, HP@360MHz(Max 400MHz), LP@40MHz.
- On-chip SRAM: 768KB L2MEM + 32KB LP SRAM + 8KB TCM.
- PSRAM: 32MB In-package stacked.
- Flash: 32MB QSPI NOR Flash.
- Display: 480x640 ST7701S MIPI panel. Firmware renders a 640x480 landscape
  logical framebuffer and rotates it to the physical panel.
- Board USB: Type-C can be routed either to USB-UART for flashing/logs, or to
  ESP32-P4 native USB HS OTG for vendor bulk transfers.

## Current Status

Implemented:

- USB-UART and USB Serial/JTAG JSON Lines handshake.
- ESP32-P4 native USB vendor bulk protocol `pet-usb-native-v1`.
- Microsoft OS 2.0 descriptors bind the native vendor interface to inbox
  WinUSB on Windows 8.1+ without an INF or Zadig installation.
- P4 capability announcement to Pet Manager, including hardware and display info.
- Stable board identity derived from the ESP32-P4 base MAC.
- Versioned, truthful capability reporting and explicit protocol ACK/NACK replies.
- Agent state, speech card, validated main/app page routing and a bounded live
  stats model retained for native widgets.
- P4 asset OTA with two on-device slots: protected built-in Terrier slot 0 plus
  one replaceable custom-appearance slot 1.
- `p4-h264-v1` playback: PC prepares 640x480 H.264 Annex-B streams when an
  appearance is built or saved, hashes the source content, and reuses one
  content-addressed `p4/families/sha256-<digest>.h264` for every family with
  identical source video bytes.
- USB-UART asset transfer at 4 Mbaud with capability-gated 8KiB raw binary
  chunks. Legacy firmware remains compatible through 3 Mbaud/921600/115200
  probing and Base64 JSON chunks.
- Firmware `0.7.36-p4` lets the desktop discover and abort an OTA transaction
  retained after a cable pull, and replaces the formerly frozen frame during
  firmware/appearance transfer with a lightweight transfer-only screen.
- Firmware `0.7.35-p4` releases the render gate and discards only incomplete
  slot-1 staging when serial or native-USB appearance traffic is idle for 15
  seconds; the previously active appearance remains intact after a host crash
  or cable disconnect.
- ESP32-P4 `esp_h264` software dual-task decoding to I420, with BT.709
  I420-to-RGB565 conversion and legacy hardware JPEG/TJPGD decoding retained
  for older MJPEG packs.
- 15 FPS full-resolution H.264 animation with a 50ms render eligibility check,
  fused PPA YUV conversion plus 270-degree rotation on the idle page, RGB565 LCD
  submission without a whole-screen RGB888 conversion, and
  rolling real-device FPS/P95 stage timing logs. Logical RGB565 stays consistent
  across UI and JPEG producers, and the ST7701S is configured for matching RGB
  element order so full-frame red/blue normalization is unnecessary.
- Two bounded PSRAM H.264 cache slots hold the content-addressed animation
  streams, eliminating per-frame SPIFFS seek/read latency during steady playback.
- Opaque session cards use preblended RGB565 shadow/selection layers instead of
  software alpha-blending hundreds of thousands of PSRAM pixels every frame.
- A state-keyed PSRAM overlay cache reuses completed session-card pixels and
  redraws only the small animated working markers on each frame.
- 64KB bounded appearance manifests; the built-in pack currently carries 16
  families with up to 225 frames per family and exact source durations.
- Optional board-safe appearance cues: PC validates PCM 16kHz mono 16-bit WAV,
  stores `p4/audio/<family>.wav`, and firmware streams it through ES8311 once
  when the selected animation family changes.
- Waveshare ESP32-P4 LCD BSP integration, ST7701S panel init, backlight and
  RGB565 rendering.
- WLK2802 write-only DSI startup: panel ID reads and command ACK requests are
  disabled so cold boot cannot deadlock the command FIFO before backlight-on.
- ST7701S hardware-reset recovery follows the datasheet's 120ms cold-start
  requirement before the initialization sequence can reach Sleep Out (`0x11`).
- MiSans Medium 16px full-CJK 1bpp font rendering, speech bubble,
  lifecycle-specific status markers, and a per-session 60-second terminal-card
  hold triggered by reliable active-set removal even when the explicit terminal
  event arrives later. Visible Session cards retain their first-seen order while
  working/thinking/tool/terminal content updates in place; new cards append and
  only hidden or expired cards leave. The runtime also supports one-shot welcome,
  rotating idle/working families, waiting-user and speaking behavior.
- Manifest parsing is bounded and happens only when assets load; the renderer
  caches selected H.264 streams in PSRAM instead of reading Flash every frame.
- Native SW1/SW2/SW3 and EC11 encoder input with pull-ups, software debounce,
  long-press detection, a bounded FreeRTOS event queue, and ten independently
  configurable gestures persisted from the PC to NVS.
- A unified local `page_toggle` action for main/app switching, plus legacy
  directional page actions and safe PC-side Agent continue/custom-prompt events.
- GT911 tap, long-press, and four-direction swipe input. Taps route to the active
  mini-app first, main-page taps trigger bounded `touch.*` reactions, and swipes
  switch between main and the installed app without resetting the LCD controller.
- Bounded P4-native declarative component runtime (`p4-bounded-runtime-v3`) shared
  by games and tools, with persisted widget state, dashboard rendering,
  transitions, ticks, app-local controls, and an optional `p4-grid-scene-v1`
  fixed-grid entity/rule layer for movement, collisions, walls, and boundaries,
  and per-file checksum ACK/retry during installation. Package files and the
  component catalog use independent A/B generations: firmware rereads and
  validates a complete candidate before one sequenced catalog snapshot commits
  widget, buttons, catalog membership, and active id together. Boot selects the
  newest fully valid snapshot and falls back to the preceding generation.
- Heap-free legacy `blocks`, `snake`, and orthodox `flappy` presets with fixed
  grids, automatic movement, bounded tick rates, semantic button controls, and
  no component code execution. Flappy applies continuous gravity and flap
  impulse while scrolling pipes, detecting collisions, and scoring completed
  passes. New components use the shared scene contract instead of selecting one
  of these presets as their engine.
- P4-native `token-usage` integration: Pet Manager removes its Linux-only reader
  declaration during P4 installation, while firmware feeds the same bounded widget
  variables from validated `stats/update` and `state/<agent>` data.
- USB PTT audio framing and validation at 16kHz mono S16LE. Firmware enables
  ES7210 capture when present, otherwise uses the ES8311 ADC/DAC path for
  board-microphone capture and playback. `audio/begin` freezes whether the visible
  Session queue was empty so the desktop can foreground the independently bound
  Codex/Claude Session. P4 voice input never falls back to the Windows microphone.
- A/B firmware OTA with two 2.5MiB slots, retry-safe 4KB acknowledged chunks, SHA-256 and
  ESP-IDF project validation, sustained 8-second health confirmation, and boot rollback.
  The application image also embeds all seven canonical components. On the
  first boot of a new build, firmware transactionally replaces same-id built-ins,
  adds missing built-ins when capacity permits, removes retired Falling Catch,
  restores the previous active component, and preserves unrelated user packages.
- Debug screenshot command over JSON Lines for framebuffer self-test.
- Persistent boot/fault-reset diagnostics with live heap, PSRAM, SPIFFS,
  GPIO/touch drop counts, touch/audio readiness, plus asset-preserving reboot/input reset tools.

Not implemented as P4-native features yet:

- Wi-Fi / MQTT runtime path on the P4 firmware.
- On-device MP4 playback. P4 accepts preprocessed H.264 Annex-B plus bounded
  PCM WAV cues.
- Full HTML/JavaScript widget runtime. P4 intentionally supports only the bounded
  declarative subset and rejects readers/fetchers.
- Local ASR/TTS.
- On-board PTT audio was hardware-validated over COM10 with ES8311 capture,
  framed USB PCM, Windows zh-CN recognition, and selected-session routing.

## Build

Windows development uses one PowerShell entry point. The one-time `setup`
command persists an ASCII-only PlatformIO core plus Python UTF-8 mode for the
current user. The default core lives under the short sibling cache
`.hachimo-tooling/platformio` beside the repository; `-PlatformIoCoreDir` may
point to another ASCII path no longer than 48 characters.

```powershell
cd esp-p4-runtime
.\tools\p4.ps1 setup
.\tools\p4.ps1 doctor
.\tools\p4.ps1 build
```

`doctor` refuses non-ASCII repository, Python, PlatformIO, or temporary paths
before CMake can generate a corrupted GCC response file. `build` runs the P4
protocol contract suite before compiling. On macOS/Linux, use the underlying
PlatformIO command directly. Factory tooling follows the active
`PLATFORMIO_CORE_DIR` when resolving both ESP-IDF `spiffsgen.py` and esptool:

```sh
cd esp-p4-runtime
python -m platformio run -e esp32_p4_evboard
```

For an existing A/B device, the guarded Windows flash action validates the
board partition table, writes bootloader/partition table/ota_0/OTA metadata,
preserves NVS, the 6.875MiB SPIFFS volume, and both 10MiB appearance partitions,
then verifies the reported firmware version over the 4 Mbaud serial protocol:

```powershell
cd esp-p4-runtime
.\tools\p4.ps1 flash -Port COM5
```

For first installation or a deliberate factory reset, build one image that
contains the bootloader, partition table, app, OTA metadata, the built-in
Terrier H.264 Annex-B/WAV appearance pack, and the seven current built-in P4
components:

```powershell
cd esp-p4-runtime
.\tools\p4.ps1 factory
```

The single flashable file is written to
`.pio/build/esp32_p4_evboard/pet-manager-p4-factory.bin`; its companion JSON
file records the image SHA-256 and segment layout. Factory flashing requires an
explicit destructive confirmation switch:

```powershell
cd esp-p4-runtime
.\tools\p4.ps1 factory-flash -Port COM5 -FactoryReset
```

The preloaded component catalog contains Two-key Pong first, followed by Flappy
Bird, Blocks, Snake, Tomato Clock, Drink Reminder, and Token Usage. The removed
Falling Catch package is not provisioned. The firmware starts on the normal pet
page; Two-key Pong is the initial component selected inside Component Center.

`factory_upload` is intentionally destructive: because the merged image starts
at `0x0`, it resets NVS, the inactive OTA slot, previous components, and both
appearance slots before provisioning the built-in Terrier into slot 0 and the
built-in component catalog. Use it for blank devices or factory recovery.
Normal Pet Manager OTA and the app-only rescue command below write only the app
partition and preserve SPIFFS appearance and user component data. After the new
app boots, its embedded component bundle performs same-id built-in migration in
the existing SPIFFS catalog; this is why a normal PC firmware update also updates
the seven built-in components without replacing the filesystem partition.

Release policy: every P4 firmware artifact distributed for a fresh install or
factory recovery must use the `pet-manager-p4-factory-v1` format and the
`pet-manager-p4-factory.bin` filename. PlatformIO's raw `firmware.bin` is only
an app-partition payload for an already provisioned device; it is not a
complete install image and must not be published as one.

Do not use the plain upload command to move a device with existing appearance
assets from the old factory+16MiB-SPIFFS layout to the current partition map.
The one-time migration first creates a verified backup, then requires explicit
factory reprovisioning because the current SPIFFS partition is smaller:

```powershell
cd esp-p4-runtime
.\tools\migrate_to_ab.ps1 -Port COM5 -FactoryReset
```

The migration script verifies that the board still has the legacy SPIFFS
partition at `0x210000`, reads all 16MB to a timestamped backup, checks its
size and SHA-256, then installs the current complete factory image. It does not
restore the old filesystem byte-for-byte into the smaller partition. The
factory image provisions the protected Terrier pack and built-in components;
the backup remains available for manual recovery or extraction. The script
refuses an already-migrated board. Keep the backup until device diagnostics,
the built-in appearance, and the component catalog have all been verified.

The PlatformIO target is configured in [platformio.ini](platformio.ini). The
current 32MiB flash layout is authoritative in [partitions.csv](partitions.csv):

| Partition | Offset | Size | Purpose |
| --- | ---: | ---: | --- |
| `nvs` | `0x009000` | 24KiB | Persistent device/input settings |
| `ota_0` | `0x010000` | 2.5MiB | Running or inactive app image |
| `ota_1` | `0x290000` | 2.5MiB | Running or inactive app image |
| `otadata` | `0x510000` | 8KiB | A/B boot selection metadata |
| `storage` | `0x520000` | 6.875MiB | SPIFFS manifests, WAV cues, components, and metadata |
| `appearance0` | `0xC00000` | 10MiB | Protected built-in H.264 appearance streams |
| `appearance1` | `0x1600000` | 10MiB | Replaceable H.264 appearance streams |

After this one-time asset-preserving migration, normal firmware updates are
started from Pet Manager's device actions menu. For USB-UART rescue on Windows,
use the same guarded action so encoding, layout validation, write verification,
and post-boot version checks stay identical:

```powershell
.\tools\p4.ps1 flash -Port COM5
```

## USB Modes

On the Waveshare ESP32-P4-WIFI6 board, the Type-C data lines are switched by
the board USB mux:

- Jumper open: Type-C routes to the CH343 USB-UART bridge. Use this for flashing,
  serial logs and rescue/debug JSON Lines.
- Jumper shorted: Type-C routes to ESP32-P4 native USB HS OTG. Use this for
  TinyUSB vendor bulk transfers, especially large appearance packs.

Power is shared by both modes; switching the data path does not change board
power input.

## Protocol

See [protocol.md](protocol.md). Runtime messages use JSON Lines:

```json
{"topic":"hello","payload":{"boardDeviceId":"p4-a1b2c3d4e5f6","runtime":"esp-p4","fw":"0.7.29-p4","buildId":"0.7.29-p4+290f402abcd1","gitSha":"290f402abcd1","buildDirty":false,"protocolSchema":5}}
```

The firmware version comes from the ESP-IDF image descriptor. Every build also
publishes its 12-character Git SHA, dirty-source marker, and protocol schema in
`hello` and `diagnostics/status`. The PC diagnostics page shows both desktop
and firmware build IDs, so equal semantic versions no longer hide different
binaries. Missing fields are treated as legacy/unknown rather than inferred.

Supported PC-to-board topics include:

- `state/<agent>`: current agent lifecycle state and status marker.
- `session/current`: current desktop-selected Session, queue position, and
  per-session title/progress/state snapshots. The P4 main page renders up to
  three task cards and applies UTF-8-safe, pixel-width `...` truncation to the
  one-line title and two-line progress preview. Existing cards keep their
  first-seen positions across every active lifecycle refresh; only their
  status/content changes in place, while newly visible Sessions append.
- `speech/text`: speech bubble title/body/status.
- `control/screen-page`: switches between the rendered `main` and installed
  mini-app `app` pages.
- `stats/update`: updates the bounded token/latency/tool metrics consumed by native widgets.
- `input/config`: validates and persists P4 button/encoder mappings; legacy
  `control/command` with `type: button_config` remains accepted. Mappings may
  use `session_next` and `session_previous`; the desktop resolves those button
  events against the current Agent session list, updates its input/state route,
  and immediately sends the selected name to the device display.
- `widget/begin`, `widget/chunk`, `widget/commit`: installs the bounded P4 `.clawpkg`
  subset; `miniapp/event` dispatches an app action and `miniapp/query` returns its
  bounded view model on `miniapp/state`. The built-in `token-usage` widget is
  compiled to this subset by the PC and receives live values from the stats model.
- `widget/delete`: advertised as `capabilities.widgetDelete=true`; it requires a
  bounded transfer id and the shared 1–47 byte lowercase component id, removes
  persisted/current mini-app state idempotently, ACKs only after cleanup, and
  returns the display to `main`. Pet Manager blocks this operation on older P4
  firmware that does not advertise the capability.
- `widget/list`: advertised as `capabilities.widgetInventory=true`; it returns
  up to 16 persisted P4 components on request-id-matched `widget/inventory`,
  including active/removable state and the exact catalog capacity. Firmware
  also advertises `componentCatalogGeneration=true` when package-level A/B
  commit and boot rollback are active.
- `asset/*`: P4 asset transfer and commit.
- `firmware/*`: acknowledged A/B firmware update, status query, abort, and commit.
- `debug/screenshot`: returns a 320x240 RGB565LE framebuffer snapshot in base64 chunks.
- `diagnostics/query`: returns reset/crash counters and live resource health.
- `system/reset-inputs`: restores the safe hardware map without deleting assets.
- `system/reboot`: performs an acknowledged delayed software restart.
- Unsupported/unknown topics: immediate `protocol/ack` NACK with an echoed `requestId` when present.

## Hardware Inputs

All controls are active-low to GND and use internal pull-ups:

| Control | GPIO | Default behavior |
| --- | ---: | --- |
| SW1 | 50 | Short press returns/cancels; long press defaults to voice input |
| SW2 | 49 | Short press opens Component Center; long press is unbound |
| SW3 | 5 | Short press confirms/enters; long press is unbound |
| Joystick key | 4 | Short press confirms; long press is unbound |
| Joystick left/right compatibility | 3/2 | Left/right retain the legacy encoder event names |

GPIO is sampled every 5ms with 25ms button debounce. Events are queued away
from rendering and transport callbacks. Applied mappings are stored as a
versioned NVS blob and survive reboot.
Pet Manager exposes SW1/SW2/SW3 short and long press plus joystick-center short
press, long press, and four directions. Joystick-center short press defaults to
`page_enter` (“确认”), while encoder long press defaults to `disabled`.
SW2 short press defaults to `component_center`, SW1 short press defaults to
`page_back` (“返回（取消）”), and SW3 short press defaults to `page_enter`.
Every gesture remains editable and persists in NVS
after downlink.
The global `page_back` binding always wins while `app` is open. Component
`buttons.json` contains only gameplay/tool actions; legacy package navigation
records are ignored, so installing or switching a component cannot overwrite
the exit key configured in Pet Manager.

## Appearance Assets

The P4 firmware does not decode MP4 on device. Pet Manager creates the following
ready pack before device synchronization:

- `p4/manifest.json`
- `p4/families/sha256-<digest>.h264`
- `p4/audio/<family>.wav` when that family has a configured cue

The built-in Terrier pack is generated during desktop packaging. Custom,
uploaded, generated, and imported appearances are converted immediately when
their source assets are saved or replaced. Device synchronization validates and
reads this ready pack only; it never invokes FFmpeg. The manifest still has one
entry per animation family, but entries whose source MP4 bytes are identical
share the same content-addressed H.264 path. The transfer
collector sends each shared device path only once.

Current export settings:

- Format: `p4-h264-v1`, raw H.264 Annex-B, Baseline-compatible SPS
- Audio format: PCM RIFF/WAVE, 16kHz, mono, 16-bit, at most 1MiB per cue
- Resolution: 640x480 canvas; source content is aspect-fitted and letterboxed
- Aspect handling: contain with black letterboxing; sources are never stretched
- Sampling FPS ceiling: 15
- Max frames per family: 225
- Timeline: frames are distributed across the complete source clip; per-family
  exact `durationMs` drives proportional frame selection; `frameDurationMs` remains a backward-compatible fallback
- H.264 quality: libx264 CRF 27, yuv420p, no B-frames, one encoder thread
  and exactly one slice per access unit for tinyh264 compatibility
- Device slots: protected built-in Terrier slot 0 plus replaceable slot 1

Current firmware advertises 4 Mbaud USB-UART and accepts a JSON
`asset/raw-chunk` header followed immediately by up to 64KiB of unencoded
binary data; the desktop deliberately uses reliable 8KiB chunks. On the 32MB
layout, slots 0 and 1 store H.264 streams sequentially in the dedicated
`appearance0` and `appearance1` raw partitions; manifests and WAV cues remain
in SPIFFS. The raw index is
committed last, before the ready marker and active-slot switch. When slot 1 is
active, Pet Manager first reactivates a valid slot 0 before a cache-miss full
sync so the replacement still targets the fast raw slot.

This path is capability-driven and shared by Windows and macOS. Pet Manager
retains the baud rate verified by the protocol handshake and logs that value,
the selected raw/Base64 transport, elapsed time, and effective KiB/s for every
sync. WCH adapters of any reported PID, plus macOS `wchusbserial` names, probe
4 Mbaud first. Old firmware keeps using 3 Mbaud/921600/115200 and compatible
Base64 JSON chunks. A CH343 Windows hardware run measured a stable 132KiB/s
for H.264 stream transfer.
Native USB vendor bulk can still reactivate an exact pack
already cached on the board. A cache miss containing H.264/MJPEG is rejected
before any full-sync erase until native USB shares the dedicated raw writer;
use USB-UART for that first transfer. On device, selected H.264
streams are cached in PSRAM and decoded sequentially; loop or family changes
restart the decoder at frame zero.

Native USB first enumerates every `303A:4040` candidate and sends each a fresh
nonce challenge. The board echoes that nonce with its `boardDeviceId`, protocol
schema, and build ID; Pet Manager selects the single exact target and repeats
the challenge after reopening it. No match, duplicate identity, stale nonce,
or old firmware without identity support is rejected before any write. Such
older firmware can continue to use the verified USB-UART path.

Normal startup never auto-formats SPIFFS. A mount failure is logged and the
volume is preserved for explicit factory recovery, preventing a transient
mount or partition mismatch from silently deleting components and metadata.

## Verification

Protocol/source contracts and the firmware build use the same sanitized Windows
environment:

```powershell
cd esp-p4-runtime
.\tools\p4.ps1 test
.\tools\p4.ps1 build
```

## Font Notice

The device UI uses MiSans Medium, downloaded from the
[official Xiaomi HyperOS font site](https://hyperos.mi.com/font/zh/), under the
MiSans Fonts Intellectual Property License Agreement. The original OTF file is
not redistributed in this repository; the checked-in LVGL bitmap is generated
at 16px, full CJK coverage, and 1bpp for the ESP32-P4 runtime.

Device smoke checks after flashing:

- Serial log includes `loaded P4 asset manifest`.
- Serial log includes `P4 H264 decoder=software-dual output=I420`. The 640x480
  profile has 30 coded macroblock rows, matching tinyh264's row-paired decoder.
  Each render pass decodes at most one frame so a temporary slowdown cannot
  turn into a multi-second catch-up stall.
- Legacy MJPEG packs may instead log `P4 JPEG decoder=hardware`.
- Serial log includes `P4 asset cache loaded slot=... bytes=...`.
- Idle playback logs `P4 H264 direct-native=PPA ... rotation=270`; pages with
  session overlays log the fallback `P4 rotation=PPA` path.
- Serial log includes `P4 render perf fps=... total=... p95=... over66=...`.
- Serial log includes `rendering P4 family=... asset=p4/families/...`.
- `debug/lcd` reports LCD init/backlight/render as `ESP_OK`.
- Serial log includes `MIPI DSI command ACK disabled for write-only panel link`
  before `Display initialized`.
- Serial log includes `inputs ready sw1=50 sw2=49 sw3=5`.
- Serial log includes `GT911 ready` and, when available, `ES8311 speaker ready`.
- SW1 emits `hold_start` only after 700ms and `hold_end` on release.
- A tap emits `screen.region.tap`; a swipe changes page; repeated render ticks do
  not replay the same family cue.
- `debug/screenshot` returns non-zero `nonBlack` and changing checksums over time.
