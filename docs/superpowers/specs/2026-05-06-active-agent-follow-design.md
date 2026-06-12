# Active Agent Follow Design

## Context

`claw-pet-manager/ref` is the Tauri 2 + React desktop manager. It owns device pairing, local agent detection, bridge profile persistence, and bundled bridge lifecycle management.

`board-runtime` is the T113 board runtime. The board subscribes to the MQTT bridge's state and speech topics:

```text
desk/<desktopDeviceId>/state/<targetSource>
desk/<desktopDeviceId>/speech/text
```

The current desktop manager supports local agent detection and a multi-select `enabledAgents` bridge profile field. The requested behavior is different: users choose exactly one primary agent CLI for the desktop pet and board to follow.

## Product Requirements

- New users see an agent-channel selection step after device/network binding succeeds and before entering the dashboard.
- The onboarding agent page is modeled after the supplied "第 3 步 / 共 3 步" layout, but it must not include the "开发 mock" section or any mock-switching controls.
- The desktop app shows fixed agent options instead of only rendering whatever the detector returns.
- Agent choice is single-select. A user can choose only one primary agent at a time.
- The dashboard exposes the same single-select control so users can change the primary agent later.
- OpenClaw is one of the fixed options and can be selected when detected.
- A disabled option remains visible when not detected, but cannot be selected.

## Fixed Agent Options

The first implementation uses this fixed display set and order:

1. `claude-code` - Claude Code
2. `codex` - Codex
3. `openclaw` - OpenClaw
4. `copilot-cli` - Copilot
5. `gemini-cli` - Gemini

Only `claude-code`, `codex`, and `openclaw` have runtime support in the current bridge. `copilot-cli` and `gemini-cli` are visible fixed options but remain disabled unless detector/runtime support exists in the same build.

## Architecture

Add `activeAgentId` to the desktop bridge profile. Treat `activeAgentId` as the source of truth for the primary agent. Keep `enabledAgents` as a backward-compatible bridge field and always write it as either `[activeAgentId]` or `[]` when no primary agent is selected.

The bundled bridge continues to publish `desk/<desktopDeviceId>/state/active`. The board runtime does not need a protocol change for this feature. Because the desktop bridge starts only the selected primary agent monitor, the active topic reflects the user's chosen agent.

## Desktop Components

### Agent Normalization

Create a shared fixed-option helper in `ref/src/agent-selection.js`.

Responsibilities:

- Define the fixed option metadata.
- Merge `detect_local_agents` results into those fixed options.
- Normalize missing options as `detected: false`, `ready: false`, `status: "not_found"`.
- Pick the default selected agent:
  - use saved `activeAgentId` when it is still selectable;
  - else use the first ready fixed option;
  - else no selection.

### Onboarding

`DeviceSetup.jsx` remains responsible for AP connection, Wi-Fi configuration, MQTT availability verification, and saving the device binding. After MQTT verification succeeds:

- load bridge profile;
- scan local agents;
- show the agent selection step;
- persist the selected primary agent;
- restart/ensure the bridge with the primary agent filter;
- then call `onComplete`.

The third step footer has:

- "上一步" to return to the prior completed verification view;
- "完成绑定" disabled until a selectable agent is chosen;
- no development mock controls.

### Dashboard

`DeviceDashboard.jsx` changes the current "编程工具" multi-checkbox list into a single-select primary-agent list.

Behavior:

- Render all fixed options.
- Selectable options use radio semantics.
- Disabled options stay visible and show "未检测到" or the detector detail.
- Selecting a different ready option saves `activeAgentId`, writes `enabledAgents: [activeAgentId]`, and restarts the bridge once after a short debounce.

## Tauri Profile Changes

Update `BridgeProfileFile`, `BridgeProfileInput`, and `BridgeProfileResponse` with:

```rust
active_agent_id: String
```

Normalization rules:

- Normalize to a known fixed agent id.
- If absent, backfill from the first existing `enabled_agents` entry.
- If still absent, keep empty.
- When saving a non-empty `active_agent_id`, store `enabled_agents` as a one-item vector containing the same id.
- Empty `active_agent_id` stores an empty vector.

Bridge startup maps only the active agent to runtime env vars:

```text
claude-code -> CLAWD_ENABLE_CLAUDE_LOG_MONITOR=true, CLAWD_SYNC_HOOKS=true
codex       -> CLAWD_ENABLE_CODEX_MONITOR=true
openclaw    -> OPENCLAW_ENABLE=true
```

All other supported bridge env vars are false for a selected different agent.

## Device Runtime

No board runtime protocol change is required for this feature. The device keeps subscribing to `state/active` by default. Existing `remote-cli-binding` remains available for cross-device or remote follow scenarios, but the local primary-agent feature is handled by the desktop bridge.

## Error Handling

- If no agent is detected during onboarding, the selection page still renders fixed disabled options and blocks "完成绑定".
- If scan fails, the page shows fixed disabled options and a retry action.
- If saving the primary agent succeeds but bridge restart fails, keep the saved selection and show an inline error asking the user to retry from the dashboard.
- If an old profile has `enabledAgents` but no `activeAgentId`, migrate to the first enabled known agent.

## Testing

Desktop Rust tests:

- old `enabled_agents: ["codex"]` backfills `active_agent_id` to `codex`;
- saving `active_agent_id: "openclaw"` persists `enabled_agents: ["openclaw"]`;
- bridge env mapping enables only the chosen primary agent;
- unknown active agent id normalizes to empty.

Frontend verification:

- build succeeds with the new shared agent selection module;
- onboarding selection step renders fixed options and no development mock area;
- dashboard uses single-select radio behavior.

Device runtime:

- no code change expected;
- existing runtime tests remain the verification gate.
