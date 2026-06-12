# 旋钮调音量 + 屏幕音量条 — 设计

**日期**: 2026-06-01
**范围**: board-runtime（设备端）+ ref（桌面客户端）
**状态**: 设计已与用户对齐，待 review

## 1. 背景与目标

设备(Raspberry Pi Zero 2 W + Google VoiceHAT)的前方编码旋钮当前旋转动作绑定的是「切屏」(`system_page`，在主屏 / 负一屏之间切换)。用户希望:

1. **旋钮旋转改为调节系统音量**(替代切屏作为默认动作)。
2. **调音量时屏幕上显示一个音量示意条**,短暂出现后自动消失。

切屏功能不丢失——仍可通过**屏幕滑动**(`board_touch_input` → `system.screen.swipe`)切页。

### 非目标 (YAGNI)
- 不做静音键 / 不做精确 dB 显示 / 不做每应用独立音量。
- 不改语音录音(capture)链路,只动播放(playback)。
- 不做图形化(像素级)音量条——经用户确认采用文字式叠加(见 §4 决策记录)。

## 2. 关键约束

- **VoiceHAT 无硬件音量控制**:`amixer -c sndrpigooglevoi scontrols` 为空。MAX98357A 是纯数字 codec,没有模拟 mixer。因此音量必须由**软件**实现。
- **宠物主屏由 ffmpeg 实时写帧**(`fb-display.sh` → rawvideo → `fb-rawvideo-blit.py` → `/dev/fb1`)。任何独立进程往同一 framebuffer 画图都会与之抢占/闪烁,故音量条不走独立 fb 写入,改为复用 ffmpeg 滤镜链里既有的**可 reload 的 drawtext 叠加机制**(与语音气泡同源)。
- **设备 CPU 紧张**(ffmpeg 常占满一核)。旋钮调音量每格 spawn 一次 `amixer` 可接受(有防抖),不引入 libasound C 依赖。

## 3. 架构总览

```
旋钮旋转 (GPIO A/B)
  └─ board_rotary_input  ── action=volume_adjust ──┐
                                                    ├─ amixer -D default sset Master ±6%   (调音量)
                                                    └─ 写 .volume-display = "<pct>\n<epoch_ms>"  (信号)
                                                                                  │
fb-display.sh (tick)  ── 读 .volume-display，<2s 新鲜 ──> 写 .volume-render = "🔊 ▰▰▰▰▰▱▱▱▱▱ 50%"
                          ── 过期 ──> 清空 .volume-render
                                                                                  │
ffmpeg 滤镜链  drawtext textfile=.volume-render:reload=1  ── 顶部居中渲染 ──> 宠物主屏

ALSA: /etc/asound.conf
  default.playback → softvol(Master) → plughw VoiceHAT     (系统级总音量，所有播放统一)
  default.capture  → plughw VoiceHAT                        (录音不变)
```

## 4. 决策记录(用户已确认)

| 决策 | 选择 | 理由 |
|---|---|---|
| 音量机制 | ALSA softvol `Master` | VoiceHAT 无硬件 mixer;softvol 系统级、持久化、一个控制点 |
| 音量范围 | 系统级总音量 | 提示音/语音回放/agent 语音统一受控,最符合"音量"直觉 |
| 音量条呈现 | 文字式条叠加在宠物主屏 | 复用现有 reload-drawtext 机制,不抢 framebuffer,最稳无闪烁 |
| 切屏去向 | 保留在屏幕滑动 | 旋钮让位给音量,切页不丢 |

## 5. 组件设计

### 5.1 ALSA softvol (`/etc/asound.conf` + `scripts/deploy-rpi.sh`)

```
pcm.!default {
    type asym
    playback.pcm "pet_softvol"
    capture.pcm  "plughw:CARD=sndrpigooglevoi,DEV=0"
}
pcm.pet_softvol {
    type softvol
    slave.pcm   "plughw:CARD=sndrpigooglevoi,DEV=0"
    control { name "Master"; card sndrpigooglevoi }
    min_dB -51.0
    max_dB   0.0
    resolution 100
}
ctl.!default { type hw; card sndrpigooglevoi }
```
- `Master` 控制首次访问时由 ALSA 自动创建,默认值由 `min/max_dB` 决定;部署时用 `amixer -D default sset Master 60%` 设一个合理初值并 `alsactl store` 持久化。
- 既有逐设备播放(fb-display `aplay -D plughw:...`)**改为走 `default`** 才能经过 softvol;否则绕过音量。这点在 §7 风险里说明。
- `deploy-rpi.sh` 里生成 asound.conf 的那段(当前直连 plughw)替换为上面带 softvol 的版本,使重刷后保留。

### 5.2 旋钮动作 `volume_adjust` (`board-runtime/src/board_rotary_input.c`)

- 新增动作字符串 `volume_adjust`,事件映射 `knob.rotate_cw / knob.rotate_ccw`(与 `negative_screen_adjust` 并列)。
- 当旋钮旋转且当前绑定为 `volume_adjust`:
  - CW → `amixer -D default sset Master 6%+ -M`;CCW → `6%- -M`(`-M` 用人耳友好的映射)。
  - 读回 `Master` 当前百分比,`br_atomic_write_text(.volume-display, "<pct>\n<now_ms>")`。
  - 用 **~150ms 防抖**(不用切屏那条 2500ms 冷却),保证跟手又不过度 spawn。
- 旋钮旋转的**默认动作**从 `system_page` 改为 `volume_adjust`(在没有 `.button-config` 或绑定缺失时的兜底)。
- 切屏逻辑(`br_rotary_emit_page` / toggle)保留,仅当绑定显式为 `system_page` 时才走。

### 5.3 音量条叠加 (`board-runtime/fb-display.sh`)

- 主循环 tick 增加:读 `.volume-display`,若 `now - ts < 2000ms` → 把可视字符串写入 `.volume-render`;否则写空串。
  - 字符串格式:`🔊 ` + 10 格 `▰`(已填)/`▱`(未填)按百分比 + ` <pct>%`。
- ffmpeg 滤镜链追加一个 drawtext:
  `drawtext=fontfile=<wqy>:textfile=<root>/.volume-render:reload=1:x=(w-text_w)/2:y=8:fontsize=18:fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=6:fix_bounds=1`
  - 插入点:在各状态构建滤镜链的公共函数里追加(与现有 `.current-speech-render` drawtext 同级),保证所有状态都带。
- `.volume-render` 初始为空 → drawtext 渲染空串(不显示),音量变化后 2s 内有内容 → 顶部居中浮现音量条。

### 5.4 客户端按钮配置 (`ref/src/DeviceDashboard.jsx`)

- `BUTTON_FUNCTION_OPTIONS` 增:`{ id: "volume_adjust", label: "音量调节", detail: "旋钮旋转调节系统音量,屏幕顶部短暂显示音量条。" }`。
- `BOARD_BUTTON_CONTROL_ROWS` 的 `encoder_rotate` 行:`actionOptions` 加 `"volume_adjust"`,`defaultAction` 改为 `"volume_adjust"`。

### 5.5 按钮配置校验放行 (`ref/src-tauri/src/lib.rs` + `board-runtime/src/board_server.c`)

- `lib.rs` `is_allowed_button_config_action`:加 `"volume_adjust"`。
- `board_server.c` `br_is_allowed_button_config_action`(及相关白名单):加 `"volume_adjust"`,使 OTA 下发的绑定通过校验并写入 `.button-config`。

## 6. 数据流 / 文件契约

| 文件 | 写者 | 读者 | 内容 |
|---|---|---|---|
| `/etc/asound.conf` | 部署 | ALSA | softvol 路由 |
| ALSA `Master` 控制 | board_rotary_input(amixer) | softvol | 0-100% 软件音量 |
| `.volume-display` | board_rotary_input | fb-display | `"<pct>\n<epoch_ms>"` 信号 |
| `.volume-render` | fb-display | ffmpeg drawtext | 可视音量条字符串(过期清空) |
| `.button-config` | board_server | rotary/touch input | 含 `volume_adjust` 绑定 |

## 7. 错误处理与风险

- **ffmpeg 滤镜链插入风险(主要)**:滤镜链是 `fb-display.sh` 里按状态动态拼的大字符串。实现时先定位公共拼接处,确认追加 drawtext 不破坏现有语音气泡且各状态都生效。**兜底**:若该链结构不适合统一插入,退化为「音量条仅在负一屏(fb-stats-renderer / Pillow)显示」,并在动手前知会用户。
- **amixer 不存在 / Master 未建**:rotary 调用前 `command -v amixer`;`sset` 失败则只记日志、跳过(不崩)。
- **绕过 softvol 的播放**:任何仍用 `-D plughw:...` 直连的播放不受音量控制。统一改走 `default`;若个别路径必须直连,在 spec 实现时标注。
- **防抖**:连续快速旋转只在 ~150ms 边界 spawn amixer,避免 CPU 抖动。

## 8. 测试

- **设备端实测**(主):
  1. 旋钮 CW/CCW → `amixer -D default sget Master` 百分比相应增减。
  2. 旋转时 `.volume-display` 更新;fb-display `.volume-render` 2s 内非空、之后清空。
  3. 屏幕顶部出现音量条并在 ~2s 后消失(肉眼 / 截帧)。
  4. 调到不同音量后播 `done.wav`,响度随之变化(系统级生效验证)。
  5. 屏幕滑动仍能切页(切屏未丢)。
- **客户端**:`npm test`(按钮配置选项快照),`cargo check`。
- **板端**:host cmake build-check + Pi 现编现部署。
- **回归**:语音 PTT 录音不受影响(capture 未动);负一屏 token widget 正常。

## 9. 改动文件清单

| 文件 | 改动 |
|---|---|
| `board-runtime/src/board_rotary_input.c` | `volume_adjust` 动作 + amixer 调音量 + 写 `.volume-display` + 默认动作 |
| `board-runtime/fb-display.sh` | tick 生成 `.volume-render` + 滤镜链 drawtext 叠加 |
| `board-runtime/scripts/deploy-rpi.sh` | asound.conf 改为 softvol 版 + 初始化 Master |
| `ref/src/DeviceDashboard.jsx` | `volume_adjust` 选项 + 旋钮默认 |
| `ref/src-tauri/src/lib.rs` | `is_allowed_button_config_action` 放行 |
| `board-runtime/src/board_server.c` | button-config 校验放行 |
| 文档 | `board-runtime/CLAUDE.md` 点文件表 + `ref/src/.folder.md` |
