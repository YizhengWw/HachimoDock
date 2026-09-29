# 随身听 / bounded media player

> 发布状态（0.1.101 / 0.7.72）：随身听暂不开放。PC 和设备组件中心隐藏入口，PC 不注册语音点歌工具、不启动音乐服务，并拒绝直接调用。源码、组件包与用户保存的播放列表保留；以下内容记录保留实现，不代表当前版本提供在线音乐服务。隐藏不改变第三方接口、内容和许可证的适用范围。

## 搜索、点歌与回执一致性

PC 不再提供音源下拉框。PC/语音默认依次搜索网易云、JOOX、哔哩哔哩；前一源返回结果即停止，仅无结果或不可用时回退，共享现有缓存与限流。限流时不继续增加请求。

每个播放器操作分别校验允许字段；`next/previous` 不能静默忽略歌名/key。语音指定歌曲须先查本轮 search/status 返回的 key，再 play；不能猜 ID。工具回执去掉封面和无关字段，纯播放器任务的最终语音依据真实 current/state 生成，queued/loading 不说已开始播放；混合待办、家居或查询任务仍由模型综合回复。日志仅记固定操作、generation 与进程内加盐的曲目关联摘要，不记歌名、原始 key 或歌词。

歌词仍为同一 music-player 包中的 SW1 长按，配置页及下发路径保留长按事件；重置按钮会清理旧版映射。以上修复不创建新组件或清空播放列表。

## Mechanism and delivery contract

The new component `music-player` is built independently. It binds the product-owned
`audio.player` media service; it cannot execute scripts, choose arbitrary URLs, or
access user files. Existing v4 packages stay compatible. Installation of a media
package additionally requires `widgetMedia=p4-media-v1`.

The complete loop is search → choose an actual result → queue → PC resolves the
stream → device buffers/plays → device reports real progress → next track or stop.
No response to a command is a claim that sound was heard. Empty, loading, buffering,
paused, playing, ended, disconnected and error states remain distinguishable.

Inputs: primary play/pause; secondary queue/player; joystick previous/next or seek
in seek mode, up/down volume or queue selection, middle seek/confirm selection.
Global exit and long-press voice bindings remain owned by the product.
Leaving the component keeps music playing in the background. From 0.7.69-p4,
pressing the configured global Back again on the pet home page stops music locally
and emits `media/event` with `operation=stop`, even during PC-side preparation.
PC cancels the current generation immediately without blocking the USB receive
thread; any cleanup request is scoped to that generation's registered session.
The library is retained. Back in realtime mode still exits the conversation first.
The global realtime action works in components and takes precedence over package
bindings, including remapped keys. SW1 long press toggles immersive lyrics in the
music component; its existing short press remains play/pause. Global realtime or
exit bound to that same gesture takes precedence. SW2 short press still opens the
queue; SW2 long press uses the user's realtime binding, not a hardcoded shortcut.
Playback opens the native music component by its stable ID, not a relative catalog
step. Firmware built-in sync includes the bundle checksum, so updated components
are installed even when two development images share the same build identity.

Visual language: warm charcoal canvas, large cover/record, orange main control,
readable title and artist, real progress with elapsed/duration, rounded controls,
and separate queue. The reusable runtime primitive is a bounded media surface,
not a browser or unrestricted HTML/CSS engine.

Music and voice must never race for the codec. Music uses a separate session ID,
independent ADPCM blocks over the existing serialized USB transport, bounded RAM
buffering, and device progress acknowledgements. Voice capture preempts music;
music never silently changes the realtime AEC sample rate. Codec changes occur
only with the microphone idle. Old chunks and responses cannot mutate a new song.

## GD 音乐台

Online discovery uses https://music-api.gdstudio.xyz/api.php . Official documentation
checked 2026-09-23 (page updated 2026-09-16) lists search, url, pic and lyric APIs,
stable sources netease/joox/bilibili and a maximum of 50 requests per 5 minutes.
The application shares a bounded rate limiter and search/metadata cache, and uses
actual returned track IDs. Stream URLs are resolved when needed, never exported.

来源：GD音乐台 https://music.gdstudio.xyz/ 。服务标注 CC BY-NC 4.0，
仅供学习参考，禁止下载、传播或商用。服务可访问不代表获得音乐版权授权。
不提供下载导出，不随组件或安装包分发音乐，不绕过登录、付费或 DRM。
音频只在播放期间进行受限流式处理。公开发布须保留说明并核实用途许可。

## Verification scope

Verify parser/URL boundaries, rate limiting, track identity, cancellation,
session/sequence checks, codec round trips, malformed block rejection, state
transitions, UI controls, old package compatibility, and both firmware targets.
Real-device sound quality, Windows serial throughput and acoustic voice handoff
must be reported separately from builds and simulated tests.

## Implemented protocol and boundaries

- PC service: `src-tauri/src/music_player.rs`; shared agent tool: `media_player`.
- UI: `src/component-center/MusicPlayer.jsx`; device surface: `pet_p4_media.c`
  and the bounded native renderer. The queue contains at most 20 stable track IDs.
- Music is decoded by FFmpeg in a child owned by the current playback generation.
  Remote bytes enter only through a bounded pipe; FFmpeg cannot fetch URLs or open
  files referenced by a remote playlist. URLs and audio are not persisted.
- Wire format `ima-block-v1`: signed LE16 predictor, step index byte, zero reserved
  byte, LE16 sample count, then low-nibble-first IMA data. Blocks hold 1–3840 mono
  samples (up to 80 ms at 48 kHz), max 1926 encoded bytes. Device decode is stateless
  between blocks; session ID and monotonically increasing sequence are required.
- `media/begin`, `chunk`, `end`, `query`, `control`, `stop`, `metadata`, `cover`,
  `library` return `media/status` with the exact request ID, actual position and
  buffered duration. Cover is optional RGB565 192×192; chunks max 2048 bytes.
  Album art is fetched asynchronously and transmitted only with audio headroom.
- `widgetLyrics=p4-lrc-v1` adds `media/lyrics`: exact session, sequential start,
  total <=256, up to 4 lines per packet, <=180 UTF-8 bytes per line, timestamps
  0..3600000 ms sorted ascending. Validation is atomic and the view appears only
  after all chunks arrive. LRC is fetched asynchronously, bounded, memory-only;
  no timed lyrics means an explicit unavailable state. Pause/seek/switch follow
  actual device position. Lyric failure never cancels audio. Icons are native
  primitives, not browser SVG or missing font glyphs.
- Media waits up to 400 ms for brief shared USB lock contention; pending media
  requests take priority over replaceable widget snapshots. No already-sent
  chunk is replayed on ACK uncertainty. Bulk transactions still serialize, and
  persistent contention remains a bounded visible failure. Logs retain timing
  and fixed error classes, not song/lyric text or stream URLs.
- The device starts after 400 ms of buffered music (or final short-track data).
  PC keeps roughly 650 ms ahead, bounded by the existing 96 KiB ring. Pausing fades
  the software music gain; voice preemption restores both codec directions to
  16 kHz before microphone capture. Music never runs the 48 kHz signal through the
  16 kHz conversation AEC. The player uses dedicated-thread macOS audio QoS/App Nap
  protection; voice transport remains unchanged.
- Control/stream requests renew a 5-second device lease; catalog refresh does not.
  Losing the PC stops playback. A queued voice play request is not reported as sound.
  Queue rows use stable keys, so duplicate titles/reordering cannot select by index.
- Local library persistence stores queue metadata and user-selected local paths
  in an owner-only file; it contains neither API credentials nor resolved URLs.
  Explicit user deletion of the queue is retained; no sample songs are bundled.

## Development checks

`cargo test --lib` covers queue edits, identity, cancellation and codec limits.
`firmware/tests/p4_media_codec_test.py` compares the actual Rust encoder, actual C
decoder and Python's independent IMA reference. `p4_media_runtime_test.py` executes
the real C protocol with simulated hardware; this is not a physical sound test.
`pc/tests/manual/music-player.html` is a development-only UI fixture using in-memory
synthetic tracks. It cannot access actual files, network services or devices.

The ignored Rust `music_player::tests::manual_device_music` test requires an explicit
serial port and a `probe`, `ota`, `play` or `remote` mode. OTA and audible testing need user
authorization; never run them as part of normal CI. It uses the production USB
manager, exact chip checks and preserve-data OTA, not factory erase.
