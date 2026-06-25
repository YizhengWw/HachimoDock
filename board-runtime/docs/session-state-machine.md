# Session 状态机设计说明

本文记录当前设备端 session 状态机逻辑。这里的 session 状态机指 `board-server` 内部的设备端状态层，而不是桌面端 bridge。

核心边界：

- 桌面端 bridge 只发布 per-source / per-session 状态事件。
- 设备端 session 状态机维护 session records、状态优先级、`done` 3 秒生命周期和最终 resolved state。
- 屏幕状态机 `fb-display.sh` 只消费 `.current-state`，不维护 session 生命周期。

## 模块分工

### 桌面端 bridge

桌面端 bridge 负责把 agent hook、日志、OpenClaw gateway 事件转换成 MQTT payload，并发布到 source topic：

```text
<namespace>/<desktopDeviceId>/state/<source>
```

示例：

```text
desk/macbook-pro/state/codex
desk/macbook-pro/state/claude-code
desk/macbook-pro/state/openclaw
```

桌面端 bridge 不负责：

- 选择全局 active session。
- 计算多个 session 的优先级。
- 维护 `done` 的 3 秒过期。
- 决定屏幕是否硬打断。

`state/active` 是历史兼容 topic。新逻辑不再依赖它，桌面端启动时会清理旧 retained active payload。

### `board-server`

`board-server` 是设备端 session 状态机和设备服务进程，负责：

- 连接 MQTT broker。
- 订阅目标桌面设备的 source 状态 topic。
- 解析并归一化状态 payload。
- 保存每个 source/session 的 record。
- 按优先级计算当前 resolved state。
- 维护 `done` 的 3 秒生命周期。
- 写入 `.current-state` 和 `.current-event`。
- 在需要即时响应时写入 `.screen-interrupt`。
- 在 USB serial 激活新形象资产时写入 `.welcome-trigger`，让显示层先播放一次欢迎视频。
- 维护 `.debug-session-state.json`。
- 接收 `speech/text` 并写入 `.current-speech`。
- 把 MQTT 状态桥接给本地 WebSocket 客户端。

### `fb-display.sh`

`fb-display.sh` 不直接订阅 MQTT。它只读取本地文件：

- `.current-state`
- `.current-event`
- `.welcome-trigger`
- `.screen-interrupt`
- `.touch-request`

因此 session 状态机和屏幕状态机之间的接口是文件接口。屏幕状态机在 clip 边界查询当前 resolved state；`.welcome-trigger` 用于新形象资产激活后的单次 `welcome` 回放，`.screen-interrupt` 和本地 touch 负责硬切换。

## MQTT Topic

默认 session 状态订阅：

```text
<namespace>/<targetDeviceId>/state/+
```

默认语音订阅：

```text
<namespace>/<targetDeviceId>/speech/text
```

默认值：

```text
namespace=desk
targetDeviceId=<配网绑定的桌面端 device id>
targetSource=<空>
```

当 `targetSource` 为空时，设备端订阅 `state/+`，并在本地维护多个 source/session 的 records。

如果配置了 `PET_CLAW_TARGET_SOURCE` 或远端绑定消息中的 `targetSource`，设备端只订阅：

```text
<namespace>/<targetDeviceId>/state/<targetSource>
```

这用于调试或兼容指定 source。正常产品路径应该让 `targetSource` 为空。

`state/active` 的处理规则：

- 默认忽略 `state/active`，避免旧 retained active 状态污染设备端 session 状态机。
- 只有设置 `BOARD_ACCEPT_LEGACY_ACTIVE=1` 时，才允许消费旧 active topic。
- `process.detected` / `process.missing` / `startup` / `heartbeat` 这类 idle 探测事件只作为 fallback；只要有真实 session record，就不会顶掉刚完成的结果框。

## 远端绑定

设备端还会订阅：

```text
<namespace>/<localDeviceId>/control/remote-cli-binding
```

合法绑定消息：

```json
{
  "command": "remote_cli_binding.update",
  "enabled": true,
  "targetDeviceId": "desktop-device-id",
  "targetSource": ""
}
```

`targetSource` 建议为空。为空表示订阅 `state/+`，由设备端 session 状态机选择 resolved state。

## 状态消息格式

状态消息可以是根对象：

```json
{
  "state": "working",
  "event": "PreToolUse",
  "reason": "codex.PreToolUse",
  "source": "codex",
  "sessionId": "session-a",
  "runId": "run-a",
  "sessionKey": "key-a",
  "tsMs": 1710000000000
}
```

也可以包在 `payload` 里：

```json
{
  "payload": {
    "state": "done",
    "event": "AssistantMessage",
    "source": "codex",
    "sessionId": "session-a",
    "tsMs": 1710000003000
  }
}
```

支持字段：

| 字段 | 作用 |
|---|---|
| `event` | 上游事件名，优先用于推导 canonical state |
| `state` | 上游状态名 |
| `rawState` | `state` 为空时的 fallback |
| `reason` | 状态原因，用于 debug |
| `source` | 状态来源，例如 `codex` / `claude-code` / `openclaw` |
| `sessionId` | 优先级最高的 session 身份字段 |
| `runId` | `sessionId` 为空时用于组成 record key |
| `sessionKey` | `sessionId` / `runId` 为空时用于组成 record key |
| `tsMs` | payload 时间戳，用于 `done` 过期和 debug |
| `screenInterrupt` | 兼容字段；为 `false` 时 push 不触发硬打断 |
| `allowInterrupt` | `screenInterrupt` 的同义兼容字段 |

record key 构造规则：

```text
<source>:session:<sessionId>
<source>:run:<runId>
<source>:key:<sessionKey>
<source>:source:<source>
```

优先使用 `sessionId`，然后是 `runId`、`sessionKey`，最后退化到 source。

## 状态归一化

设备端最终只向屏幕状态机输出这些 canonical state：

- `idle`
- `working`
- `waiting_user`
- `done`
- `error`

`welcome` 和 `touch` 不属于 session 主状态：

- `welcome` 由屏幕状态机根据配网完成事件处理。
- `touch` 是屏幕本地抢占事件，由 `.touch-request` 触发。

### Event 映射

| Event | Canonical state |
|---|---|
| `UserPromptSubmit` | `working` |
| `PreToolUse` | `working` |
| `SubagentStart` | `working` |
| `PreCompact` | `working` |
| `WorktreeCreate` | `working` |
| `PostToolUse` | `working` |
| `SubagentStop` | `working` |
| `AssistantMessage` | `done` |
| `Stop` | `done` |
| `PostCompact` | `done` |
| `PostToolUseFailure` | `error` |
| `StopFailure` | `error` |
| `Elicitation` | `waiting_user` |
| `PermissionRequest` | `waiting_user` |
| `Notification` | `waiting_user` |
| `SessionStart` | `idle` |
| `SessionEnd` | `idle` |

### State 映射

| 上游 state | Canonical state |
|---|---|
| `idle` | `idle` |
| `working` | `working` |
| `active` | `working` |
| `thinking` | `working` |
| `tool_running` | `working` |
| `speaking` | `working` |
| `waiting_user` | `waiting_user` |
| `notification` | `waiting_user` |
| `done` | `done` |
| `error` | `error` |

未识别的 event / state 不会写入 session record。

## Active 选择

设备端 session 状态机在所有未过期 record 中选择当前 resolved state。选择规则是 **最近更新的 session record 获胜**，这与桌面端 Codex 宠物的多 session 展示逻辑保持一致。

说明：

- `tool_running` / `speaking` / `thinking` 会先归一化为 `working`。
- 不再按 `error > waiting_user > done > working > idle` 做状态优先级抢占。
- 同一个 source 可以同时保留多个 session record；新 session 不会清理同 source 旧 session。
- 如果一个较新的 session 从 `waiting_user` 更新到 `working`，屏幕会显示 `working`，即使另一个较旧 session 仍是 `waiting_user`。
- 没有可用 record 时 resolved state 为 `idle`，reason 为 `session.no_sources`。

## Done 状态

`done` 是短生命周期状态，由设备端 session 状态机维护。

规则：

1. 收到 `done` record 后，写入 `displayUntilMs = tsMs + BOARD_SESSION_DONE_HOLD_MS`。
2. `BOARD_SESSION_DONE_HOLD_MS` 默认 `3000ms`。
3. `done` 在过期前像其他状态一样参与“最近更新者获胜”选择。
4. `done` 到期后从 records 中移除。
5. 移除后重新计算 resolved state。
6. `done` 过期只更新 `.current-state`，绝不写 `.screen-interrupt`。

如果 `done` 到期后还有其他 session 正在 `working`：

```text
done -> working
```

这只是 resolved state 自然变化。屏幕状态机不会在第 3 秒被强制打断，而是在当前 `done` clip 播放完成后读取新的 `.current-state`。

如果没有其他 record：

```text
done -> idle
```

如果设备首次启动时收到 retained 的旧 `done`，并且 `now >= tsMs + BOARD_SESSION_DONE_HOLD_MS`，设备端会忽略这个过期 `done`，避免开机后显示历史完成状态。

## 硬打断

设备端只有在 MQTT push 导致 resolved state 发生变化时，才可能写 `.screen-interrupt`。

当前硬打断条件：

```text
payload 允许 interrupt
并且 push 让 resolved state 变化到 done 或 error
```

具体行为：

- 进入 `done`：写 `.screen-interrupt`，尽快展示完成反馈。
- 进入 `error`：写 `.screen-interrupt`。
- `working`、`waiting_user`、`idle`：不写 `.screen-interrupt`，等当前 clip 边界自然切换。
- `done` 到期后的自然降级：不写 `.screen-interrupt`。

`screenInterrupt:false` / `allowInterrupt:false` 只影响当前 push 是否允许硬打断，不影响 session record 的保存和 resolved state 计算。

## Waiting User 状态

`waiting_user` 表示 Agent 暂停工作，等待用户确认或输入。

当前规则：

- 收到 `waiting_user` record 后参与“最近更新者获胜”计算。
- 不写 `.screen-interrupt`。
- 不做独立短时过期。
- 当后续收到 `working`、`done`、`error` 等新 record 后重新计算 resolved state。

如果用户再次发送消息并触发 `working`，设备端会从 `waiting_user` 进入 `working`，屏幕在当前 clip 边界自然切换。

## Error 状态

`error` 是高优先级状态。

当前规则：

- 收到 `error` record 后参与“最近更新者获胜”计算。
- 如果 resolved state 进入 `error`，写 `.screen-interrupt`。
- 连续收到同一 resolved `error` 不重复打断。
- 当前不做独立短时过期，后续恢复由新 session event 驱动。

## 状态写入

当 session 状态机计算出新的 resolved state 后，`board-server` 写入：

```text
.current-state
.current-event
.debug-session-state.json
```

`.current-state` 示例：

```text
working
```

`.current-event` 写入当前 active record 的 event；没有 event 时为空字符串。

`.debug-session-state.json` 示例：

```json
{
  "resolvedState": "working",
  "resolvedEvent": "PreToolUse",
  "activeSessionKey": "codex:session:session-a",
  "lastReason": "codex.PreToolUse",
  "updatedAtMs": 1710000000000,
  "records": [
    {
      "sessionKey": "codex:session:session-a",
      "source": "codex",
      "state": "working",
      "event": "PreToolUse",
      "seq": 0,
      "updatedAtMs": 1710000000000,
      "displayUntilMs": 0,
      "candidate": true
    }
  ]
}
```

这里的 `records` 是设备端真实 session records，用于现场判断当前状态来自哪个 source/session。

## 配网状态覆盖

配网状态不是 MQTT session record，但它会复用 `.current-state`，让屏幕展示配网反馈。

当前规则：

| Pairing state | 写入状态 | 写入 event |
|---|---|---|
| 等待配置 / 发现 / AP fallback | `waiting_user` | `PairingWaiting` |
| 配网完成且 AP 仍 active | `idle` | `PairingReady` |
| 配网完成且 AP 不 active | `idle` | 空 |

`PairingReady` 会被屏幕状态机识别为欢迎动作入口，播放 `welcome.mp4` 后再进入正常 idle / session 状态。

## Speech 处理

状态类字幕不由 `board-server` 在收到状态消息时立即写入。它只写 `.current-state` / `.current-event`，由 `fb-display.sh` 在真正进入对应 clip 时写 `.current-speech`。

这保证 `done` 字幕只会和 `done.mp4` 同步出现，而不会在 MQTT 状态刚到达、屏幕仍在播放 `working.*` 时提前显示。

如果收到 speech topic：

```text
<namespace>/<targetDeviceId>/speech/text
```

则解析其中的 `displayTitle` / `sessionTitle` / `displayContent` / `content` / `text` 和 session 身份字段，更新 per-session speech records 后重新写入 `.current-speech`。

现在 speech topic 会保留最近 4 个 session 的回复记录，而不是只保留最后一条全局文本。记录 key 与状态 record 一致：

```text
<source>:session:<sessionId>
<source>:run:<runId>
<source>:key:<sessionKey>
<source>:source:<source>
```

每次收到新的 speech payload，设备端会更新对应 session 的回复，并按 `updatedAtMs` 从新到旧合并写入 `.current-speech`，例如：

```text
修 UI 动画: 已经改好了状态选择逻辑
调试部署脚本: SD 卡打包完成
```

每条 speech 默认保留 `BR_SPEECH_HOLD_MS=30000ms`，也就是 30 秒；如果 payload 带 `expiresAtMs` 且仍在未来，则使用 payload 的过期时间。

## Debug 接口

查询当前 debug 状态：

```http
GET /debug/state
```

返回内容会合并：

- `.debug-session-state.json`
- `.debug-screen-state.json`
- debug overlay 开关状态

打开顶部 debug overlay：

```http
POST /debug/overlay
{"enabled":true}
```

debug overlay 会显示 session state 和 screen state 两部分。

## 边界说明

设备端 session 状态机负责：

- 多 source/session records。
- 最近更新 session 的 active 状态选择。
- `done` 3 秒过期。
- resolved state debug 快照。

设备端 session 状态机不负责：

- 播放视频。
- 决定 clip variant。
- touch 本地抢占。
- welcome 首次欢迎动作。
- 生成或修改视频素材。
