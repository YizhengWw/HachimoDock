# 屏幕状态机设计说明

本文记录设备端屏幕状态机的当前实现逻辑。这里的“屏幕状态机”主要指 `fb-display.sh`，它负责把 session 状态机输出的业务状态转换成屏幕上实际播放的视频 clip。

## 模块分工

### `board-server`

`board-server` 内部包含设备端 session 状态层，是 session 状态机和屏幕状态机之间的桥接层，负责：

- 接收 MQTT 状态消息。
- 归一化业务状态，写入 `.current-state` 和 `.current-event`。
- 在需要屏幕硬打断时写入 `.screen-interrupt`。
- 维护 debug 快照文件，供 `/debug/state` 查询。
- 提供 `/debug/overlay` 开关顶部 debug 显示。

屏幕状态机不直接订阅 MQTT，只通过这些本地文件和 `board-server` 交互。

### `fb-display.sh`

`fb-display.sh` 是真正的屏幕状态机，负责：

- 扫描 `terrier-clips` 下的视频文件。
- 根据文件名动态建立 state -> clip 列表。
- 读取 `.current-state` 判断当前 session 最高优先级状态。
- 在 clip 边界进行软切换。
- 在 `.screen-interrupt` 或本地 touch 请求出现时进行硬打断。
- 在 Raspberry Pi 上控制 ffmpeg 解码视频，并通过 `fb-rawvideo-blit.py` 写入 framebuffer。
- 在真正开始播放某个 clip 时写入对应的状态字幕 `.current-speech`。
- 写入 `.debug-screen-state.json` 和 `.current-debug-speech`。

### `board-touch-input`

`board-touch-input` 负责读取 `/dev/input` 的触摸事件：

- 上报 touch action 到 MQTT。
- 同时写入本地 `.touch-request`，让屏幕状态机立即播放 touch clip。

touch 是屏幕本地抢占事件，不属于 session 主状态。

### `fb-speech-overlay`

`fb-speech-overlay` 负责在 framebuffer 顶部叠加文字：

- 普通模式下显示 `.current-speech`。
- debug overlay 开启时显示 `.current-debug-speech`。

debug 内容包括屏幕状态机状态和 session 状态机状态。

## 状态模型

屏幕只识别固定 state 集合：

- `welcome`
- `idle`
- `working`
- `waiting_user`
- `done`
- `error`
- `touch`

业务层只需要关心是否处于 `working`，不需要指定 `working.typing`、`working.browsing` 这样的 variant。

后续允许增加新视频，但只能增加这些 state 下的 variant，不能增加新的 state。

## Clip 命名和加载

所有视频放在 `terrier-clips` 目录下，使用扁平文件名：

```text
<state>[.<variant>].mp4
```

示例：

```text
idle.playing.mp4
idle.wandering.mp4
working.typing.mp4
working.browsing.mp4
touch.lick.mp4
```

启动时 `fb-display.sh` 会扫描 `terrier-clips/*.mp4`，通过文件名前缀识别 state：

- `working.typing.mp4` 属于 `working`。
- `idle.happy.mp4` 属于 `idle`。
- `touch.wave.mp4` 属于 `touch`。

未知 state 会被忽略。

## 播放器控制

当前 Raspberry Pi 实现使用短生命周期 ffmpeg 播放进程：

1. `fb-display.sh` 选择当前 clip。
2. 启动 ffmpeg，把视频缩放/填充到屏幕尺寸并输出 rawvideo。
3. `fb-rawvideo-blit.py` 把 rawvideo 写入 `/dev/fb1` 等 framebuffer。
4. 屏幕状态机仍根据 clip 时长决定“一轮播放”何时结束。

这样播放器实现可以跟随 Raspberry Pi 的标准用户态工具链，不依赖旧平台的专用播放器。

## 状态字幕同步

状态类字幕由 `fb-display.sh` 在 clip 真正开始播放前写入 `.current-speech`，而不是由 `board-server` 在收到 MQTT 状态推送时立即写入。

这样可以保证：

- `.current-state` 只是 session 期望状态。
- `.current-speech` 跟屏幕实际播放的 clip 对齐。
- `working -> done` 这类软切换不会提前出现 `搞定啦！`。
- touch 字幕跟随实际随机到的 `touch.*` clip。

`board-server` 仍会在两类非状态字幕场景写 `.current-speech`：

- 配网等待提示，例如“请打开电脑端 HachimoDock（哈基米机）进行配网。”
- 上游 `speech/text` topic 推送的实际文本内容；如果 payload 带 `source` + `sessionId` / `runId` / `sessionKey`，设备端会按 session 保留最近回复 30 秒，并按更新时间合并显示。

配网等待期间，`fb-display.sh` 不会用通用 `waiting_user` 文案覆盖配网提示。

## Clip 时长

播放窗口按以下优先级获取：

1. `terrier-clips-durations.tsv`
2. 设备端 `python3` 解析 mp4 `mvhd`
3. `PET_CLAW_FB_CLIP_MAX_SECONDS` 兜底

实际播放窗口会从原始 duration 中减去 `PET_CLAW_FB_CLIP_EDGE_TRIM_SECONDS`，默认 `0.08s`，避免卡在尾帧。

默认 duration 表由部署流程同步到设备：

```text
terrier-clips-durations.tsv
```

新增视频后需要更新这个表，或者保证设备端 `python3` 能解析新 mp4。

## 循环规则

每次进入一个 state 时：

1. 从该 state 的 clip 池中随机选择一个 clip。
2. 根据 state 的最大循环次数随机生成本轮目标次数。
3. 播放当前 clip，一次完整 clip 结束后才增加 loop。
4. 达到本轮目标次数后，再从该 state 的 clip 池中随机选择下一个 clip。

默认配置：

```text
PET_CLAW_FB_IDLE_MAX_LOOPS=5
PET_CLAW_FB_WORKING_MAX_LOOPS=5
```

含义是进入 `idle` 或 `working` 时，当前 variant 会随机播放 `1..5` 次，然后再随机切换到同 state 下的下一个 variant。

如果某个 state 只有一个 clip，例如当前的 `done`、`error`、`waiting_user`，它会重复播放该 clip，直到 session 状态机在 clip 边界读取到新的状态。

## 软切换

软切换表示不打断当前 clip，而是在当前 clip 完整播放结束后读取 session 状态机的当前最高状态，然后决定下一个 clip。

典型场景：

- `working` -> `waiting_user`
- `working` -> `done`
- `done` -> `idle`
- `idle.<variant>` -> `idle.<nextVariant>`
- `working.<variant>` -> `working.<nextVariant>`

这里的“完整播放结束”由 clip duration 决定，不由 ffmpeg 进程退出时间决定。

`done` 的 3 秒过期由设备端 session 状态机维护。屏幕状态机不会为了精确 3 秒打断 `done` 视频，而是在当前 `done` clip 播放完成后查询 session 当前状态，再切到新的状态。

## 硬切换

硬切换表示立即停止当前 clip，丢弃当前播放计划，然后重新读取 session 当前最高状态并进入目标 state。

当前由 `.screen-interrupt` 驱动。`board-server` 在以下场景写入 `.screen-interrupt`：

- 从非 `working` 进入 `working`
- 进入 `error`

`done` 自然过期后如果露出 `working`，`board-server` 只更新 `.current-state`，不会写 `.screen-interrupt`。这样可以避免第 3 秒强行打断 `done` 视频。

屏幕状态机收到 `.screen-interrupt` 后不会信任 marker 内的状态名，而是重新读取 `.current-state`，以 session 状态机当前最高状态为准。

## Touch 处理

touch 是屏幕本地抢占事件，来源于 `.touch-request`。

规则：

- 当前 session 处于 `waiting_user` 或 `error` 时忽略 touch。
- 其他状态下收到 touch，立即打断当前 clip。
- 随机播放一个 `touch.*` clip。
- touch clip 播放完后重新查询 session 状态机，进入当前最高状态。

当前默认 touch clip：

```text
touch.lick.mp4
touch.what.mp4
```

## 配网完成和 welcome

`welcome` 是特殊状态，只用于配网流程完成后的欢迎动作。

`board-server` 在配网状态从等待态进入 `PairingReady` 时，屏幕状态机记录一次 `welcome` checkpoint：

```text
welcome -> idle.<random>
```

`welcome` 播放完成后，屏幕状态机继续按 clip 边界读取 session 当前状态。

## Debug 模式

debug overlay 可通过 HTTP 打开：

```http
POST /debug/overlay
{"enabled":true}
```

关闭：

```http
POST /debug/overlay
{"enabled":false}
```

查询：

```http
GET /debug/state
```

debug overlay 打开后，屏幕顶部会显示两行：

```text
screen state=<displayedState> clip=<currentClip> loop=<current>/<target>
session state=<desiredState> event=<event>
```

这些信息来自：

- `.debug-screen-state.json`
- `.debug-session-state.json`
- `.current-debug-speech`

## 关键配置

| 配置 | 默认值 | 说明 |
|---|---:|---|
| `PET_CLAW_FB_IDLE_MAX_LOOPS` | `5` | `idle` variant 随机播放最大次数 |
| `PET_CLAW_FB_WORKING_MAX_LOOPS` | `5` | `working` variant 随机播放最大次数 |
| `PET_CLAW_FB_CLIP_MAX_SECONDS` | `30` | duration 解析失败时的兜底最大播放时间 |
| `PET_CLAW_FB_CLIP_EDGE_TRIM_SECONDS` | `0.08` | clip 尾部裁剪时间 |
| `PET_CLAW_FB_DURATION_FILE` | `$RUNTIME_ROOT/terrier-clips-durations.tsv` | clip duration 表路径 |
| `PET_CLAW_FB_DEBUG_OVERLAY` | `0` | 启动时是否默认开启 debug overlay |
| `PET_CLAW_FB_DISABLE_CACHE` | `0` | 是否禁用 `/tmp/fb-videos` 缓存 |

## 当前默认 clip 集合

默认最小可用集包含：

```text
welcome.mp4
idle.playing.mp4
idle.wandering.mp4
working.browsing.mp4
working.typing.mp4
waiting_user.mp4
done.mp4
error.mp4
touch.lick.mp4
touch.what.mp4
```

当前仓库实际还包含更多 `idle.*` 和 `working.thinking.mp4`，都会被动态加入对应 state 的随机池。
