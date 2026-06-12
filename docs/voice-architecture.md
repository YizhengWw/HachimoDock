# Voice Architecture

> Status: **Design (rev 4)** · Last reviewed 2026-05-26
>
> Owners: voice-service-node · claw-pet-manager · openclaw-runtime
>
> This document is the contract between the three components above. If you
> change anything in voice-service / pet-manager / openclaw that affects this
> contract, update this file in the **same** PR.
>
> ### Revision history
> - **rev 1 (2026-04-28)**: initial design; assumed OpenClaw needed a new
>   WebSocket `sessions.inject` method.
> - **rev 2 (2026-04-28)**: corrected after reading
>   `pet-claw/src/application/clients/pet-claw-{client,agent-helper}.{js,mjs}`.
>   OpenClaw is **not** controlled via WS — it is an npm package whose
>   `dist/plugin-sdk/agent-runtime.js` exports `agentCommand()`. So
>   OpenClawAdapter mirrors pet-claw's helper-process pattern instead of
>   asking OpenClaw to ship a v4 protocol. Also: session "freshness window"
>   removed — `resolveActive()` always picks the latest existing session.
> - **rev 3 (2026-05-26)**: Pet Manager board-audio enable now performs
>   one-click runtime preflight (`ensure_bridge_runtime` then
>   `ensure_voice_runtime`) before sending `audio_bridge`, targets the current
>   online board id before falling back to the saved binding id, and exposes
>   board voice enablement plus trigger-button configuration with an explicit
>   USB OTA affordance. Pet Manager passes `voiceButton` through
>   `audio_bridge_signal`; the Rust command writes `voice_button` into the
>   board control payload and attempts USB first when the device is connected.
> - **rev 4 (2026-05-26)**: clarified that push-to-talk is board-side. Pet
>   Manager only stores/enables the board voice setting, lets the user choose
>   which physical board button is the voice button, and sends that config to
>   the board over USB first, MQTT second. The board runtime owns microphone
>   capture, playback listener startup, and hardware button press/release
>   handling.

## 1. The one-line goal

**Voice is just another I/O modality for the user's existing agent
session.** When the user picks up a microphone (Mac built-in, Bluetooth
headset, T113 board mic, …) and says "那直接帮我改吧", that sentence must
land in the **same agent session** they were typing into a moment ago, with
the **same context, same model, same tool permissions**. Audio is transport;
the agent is still the agent.

What this rules out:

- **No** "voice has its own LLM" — `voice-service-node` does not pick
  models, does not own a `defaults.llm` provider, does not negotiate any
  chat-completions endpoint.
- **No** silent fallback to a generic LLM — if the user's selected agent
  isn't available, voice reports failure clearly; we don't quietly answer
  with a different brain.

## 2. High-level dataflow

```
┌──────────────────────────────────────────────────────────────────────┐
│                              USER                                     │
│  ┌──────────────┐         ┌──────────────────────────────┐            │
│  │ IDE keyboard │         │  Mic / Speaker               │            │
│  │  (Claude /   │         │  (Mac built-in,              │            │
│  │   Codex /    │         │   T113 audio_bridge UDP)     │            │
│  │   OpenClaw)  │         └────────────┬─────────────────┘            │
│  └──────┬───────┘                      │ audio                         │
└─────────┼──────────────────────────────┼─────────────────────────────┘
          │                              │
          │             ┌────────────────▼───────────────────┐
          │             │  voice-service-node                │
          │             │  ─ STT (volc) → text               │
          │             │  ─ TTS (volc) ← text               │
          │             │  ─ NO LLM here                     │
          │             └────────────────┬───────────────────┘
          │                              │ POST /agent/inject  (SSE stream)
          │                              ▼
          │             ┌────────────────────────────────────┐  ★ NEW
          │             │  Agent Session Bus                 │
          │             │  (in pet-manager bridge,           │
          │             │   port 8181 by default)            │
          │             │                                    │
          │             │  · resolveSession(agentId)         │
          │             │  · inject(agentId, sid, text)      │
          │             │     → SSE { token | tool | done    │
          │             │              | error }             │
          │             └─┬───────────┬──────────────┬───────┘
          │               │           │              │
          │               ▼           ▼              ▼
          │      ┌──────────────┐ ┌─────────┐ ┌──────────────────┐
          │      │ ClaudeCode   │ │ Codex   │ │ OpenClaw         │
          │      │ Adapter      │ │ Adapter │ │ Adapter          │
          │      │              │ │         │ │                  │
          │      │ spawn        │ │ spawn   │ │ spawn helper.mjs │
          │      │ `claude -p   │ │ `codex  │ │ → import         │
          │      │  --resume    │ │  exec   │ │   openclaw/dist/ │
          │      │  --output-   │ │  --re-  │ │   plugin-sdk/    │
          │      │  format      │ │  sume   │ │   agent-runtime  │
          │      │  stream-json`│ │  ...`   │ │ → agentCommand() │
          │      └─────┬────────┘ └────┬────┘ └────────┬─────────┘
          │            │               │               │
          ▼            ▼               ▼               ▼
   user's agent     ~/.claude/      ~/.codex/      ~/.openclaw/
   process(es)      projects/      sessions/       agents/<id>/
   (the same one    <encoded-cwd>/  <date>/         sessions/
    the IDE talks   <sid>.jsonl    <sid>.jsonl     sessions.json
    to)                                            + rollout.jsonl
```

> The OpenClaw row is the one that changed in rev 2. There is **no** WS
> `sessions.inject` call — the existing `connect`/`sessions.subscribe` WS
> channel stays read-only (used today by `OpenClawGatewayBridge` to mirror
> state to the board); the *write* path goes through OpenClaw's local
> npm-installed `agent-runtime` module, exactly the way `pet-claw` already
> talks to it from a desktop pet.

Key invariants:

| Invariant | Why |
|---|---|
| Session is owned by the agent, not by voice. | The IDE and the mic write into the same `~/.claude/.../<sid>.jsonl`. They share history, tool state, model choice. |
| Bus is a routing/adaptation layer, not a session store. | No per-session state in the bus beyond an open SSE connection. Restart-safe. |
| voice-service-node has no LLM concept. | Removes the "voice answers with mimo even though I'm a Claude user" failure mode for good. |
| Agent unavailable → hard error surfaced to UI. | The user's expectation is "talk to my agent"; silently switching brains violates that contract. |

## 3. The Agent Session Bus

Lives inside `claw-pet-manager` (Tauri Rust + bridge Node sidecar). Listens
on `127.0.0.1:8181` (configurable via `AGENT_BUS_PORT`).

### 3.1 HTTP / SSE contract

**`POST /agent/inject`**

```jsonc
// request body
{
  "agentId": "claude-code" | "codex" | "openclaw",
  "sessionId": "auto" | "<adapter-specific-id>",
  // "auto" = resolveSession() picks latest active or opens a new one
  "text": "<utterance from STT>",
  "metadata": {
    "source": "voice",
    "stt": { "provider": "volc", "confidence": 0.92 },
    "locale": "zh-CN"
  }
}
```

Response: **Server-Sent Events** stream, one event per agent action.

```text
event: token
data: {"text": "好"}

event: token
data: {"text": "的"}

event: tool
data: {"name": "Edit", "input": {...}, "phase": "start"}

event: tool
data: {"name": "Edit", "phase": "end", "ok": true}

event: token
data: {"text": "改"}

event: done
data: {"sessionId": "abc123", "tokens": 42, "stopReason": "end_turn"}
```

Failure cases:

```text
event: error
data: {"code": "AGENT_UNAVAILABLE", "message": "Claude Code CLI not found in PATH"}

event: error
data: {"code": "SESSION_NOT_FOUND", "message": "No active session and openNew failed"}
```

### 3.2 Other endpoints

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/agent/status` | `{ agentId, ready, sessions: [...] }` for each known agent |
| `GET` | `/agent/sessions?agentId=...` | List sessions (id, lastModified, cwd, summary) for the UI dropdown |
| `POST` | `/agent/cancel` | Cancel an in-flight `inject` (the agent run keeps going if it's mid-tool, this only cancels the SSE) |

### 3.3 The Adapter interface (TypeScript)

```ts
interface AgentAdapter {
  readonly agentId: "claude-code" | "codex" | "openclaw";

  /** Probe whether this agent is installed/configured on this machine. */
  isAvailable(): Promise<{ ready: boolean; reason?: string }>;

  /** List sessions usable by `inject` — most-recent first. */
  listSessions(opts?: { limit?: number }): Promise<SessionRef[]>;

  /** Pick the session to use for `auto` resolution. May return null. */
  resolveActive(): Promise<SessionRef | null>;

  /** Open a brand-new session and return its ref. */
  openNew(opts?: { cwd?: string }): Promise<SessionRef>;

  /** Inject one user turn into a session and stream agent output. */
  inject(req: InjectRequest): AsyncIterable<AgentEvent>;
}

interface SessionRef {
  id: string;
  lastModified: number; // epoch ms
  cwd?: string;
  summary?: string; // short label for UI
}

interface InjectRequest {
  sessionId: string;
  text: string;
  metadata?: Record<string, unknown>;
  signal?: AbortSignal;
}

type AgentEvent =
  | { kind: "token"; text: string }
  | { kind: "tool"; name: string; phase: "start" | "end"; input?: any; ok?: boolean }
  | { kind: "done"; sessionId: string; tokens?: number; stopReason?: string }
  | { kind: "error"; code: string; message: string };
```

## 4. Per-agent adapter recipes

### 4.1 ClaudeCodeAdapter

| Concern | Implementation |
|---|---|
| `isAvailable()` | `which claude`; verify `claude --version` returns ≥ 1.x |
| `listSessions()` | Walk `~/.claude/projects/<encoded-cwd>/*.jsonl`, sort by mtime desc; the encoded-cwd is the URL-safe form of the project dir, same as Claude Code itself uses |
| `resolveActive()` | **Always** the newest session in `listSessions()` — no time-based freshness window. If the user's last session is from yesterday we still continue it. Return `null` only when no session exists at all → caller falls through to `openNew()`. |
| `openNew(cwd)` | First call to `claude -p "<text>" --output-format stream-json` without `--resume` opens a session; we capture the new sid from the first emitted event |
| `inject(sid, text)` | `claude -p "<text>" --resume <sid> --output-format stream-json --verbose`; spawn child, parse stream-json line-by-line |
| stream-json mapping | `assistant`/`message` text deltas → `{kind:"token"}`; `tool_use` start/end → `{kind:"tool"}`; final `result` event → `{kind:"done"}`; non-zero exit → `{kind:"error"}` |
| concurrency | Only one inject in-flight per sessionId; concurrent calls queue and run serially (Claude doesn't tolerate parallel writes to one session) |

### 4.2 CodexAdapter

| Concern | Implementation |
|---|---|
| `isAvailable()` | `which codex`; verify `codex --version` returns a version supporting `exec` subcommand and `--output-format stream-json` (Codex CLI 0.40+) |
| `listSessions()` | Walk `~/.codex/sessions/<yyyy-mm-dd>/<uuid>.jsonl` (or whatever the active layout is), sort by mtime desc |
| `resolveActive()` | **Always** the newest session in `listSessions()` — same "永远续最近" rule as ClaudeCode. Return `null` only when there are zero sessions. |
| `openNew(cwd)` | `codex exec "<text>"` (no `--resume`); capture sid from first event |
| `inject(sid, text)` | `codex exec "<text>" --resume <sid> --output-format stream-json` |
| stream-json mapping | Same as Claude (Codex's stream-json is intentionally similar) |
| concurrency | Same serial-per-session rule |

### 4.3 OpenClawAdapter

> **Important context**: OpenClaw exposes two completely separate channels:
>
> 1. **Read channel — WebSocket gateway** at `ws://127.0.0.1:18789`.
>    `OpenClawGatewayBridge` (in pet-manager bridge `headless-mqtt.js`)
>    already negotiates `connect` + `sessions.subscribe` here and forwards
>    events as MQTT topics so the board can mirror state.
>    **This channel is read-only by design.** It tells you *what the agent
>    is doing*; it does not let you *make the agent do something*.
>
> 2. **Write channel — local npm module**. OpenClaw is installed as the npm
>    package `openclaw` (CLI binary `openclaw`). Once installed, its
>    `dist/plugin-sdk/agent-runtime.js` exports `agentCommand({message,
>    sessionId, agentId, ...})`. `pet-claw` already drives it this way via
>    `pet-claw-agent-helper.mjs` — the helper imports the runtime module,
>    calls `agentCommand`, and pipes events back over stdout. Voice does
>    the same thing. **No WS protocol change needed.**

Resolved paths (from `pet-claw-path-resolver.js` — copy that resolver, do
not reinvent):

| Resource | Location |
|---|---|
| Runtime entry  | `<openclaw-pkg>/dist/index.js` |
| Helper module  | `<openclaw-pkg>/dist/plugin-sdk/agent-runtime.js` exports `agentCommand` |
| Event source   | sibling `agent-events-*.js` / `pi-embedded-*.js` exports `onAgentEvent` |
| Sessions index | `~/.openclaw/agents/<agentId>/sessions/sessions.json` |
| Per-session log| `~/.openclaw/agents/<agentId>/sessions/<sid>.jsonl` |

Adapter mapping:

| Concern | Implementation |
|---|---|
| `isAvailable()` | `pathResolver.detect()` returns a runtime; check the agent-runtime module file actually exists. If not, mark unavailable (the user has the WS gateway up but no npm package — typical for a partial install). |
| `listSessions()` | Read `~/.openclaw/agents/<agentId>/sessions/sessions.json` (already a list, OpenClaw maintains it for itself), enrich with mtime of each `<sid>.jsonl`, sort desc. Fall back to `fs.readdir` of the sessions dir if the index file is missing. |
| `resolveActive()` | **Always** the newest entry in `listSessions()` — same "永远续最近" rule. Return `null` only when no session exists. |
| `openNew(cwd)` | Spawn helper with `agentCommand({ message, agentId, sessionId: undefined, cwd })`. OpenClaw allocates a new `sid` and emits it on the first event; we capture and return it. |
| `inject(sid, text)` | Spawn helper with `agentCommand({ message: text, agentId, sessionId: sid })`. The helper subscribes to `onAgentEvent(callback)`, filters by `runId`, and emits agent events on stdout as `@@PET_CLAW@@{...}` lines (we'll rename the marker to `@@VOICE_BUS@@`). The adapter consumes those lines and yields `AgentEvent`s. |
| Event mapping  | `event.type === 'token'` → `{kind:"token"}`; `tool_use_start` / `tool_use_end` → `{kind:"tool"}`; final `result` payload → `{kind:"done", sessionId, tokens}`; helper-level error → `{kind:"error"}` |
| concurrency    | Same serial-per-session rule; OpenClaw's runtime lets concurrent runs into different sessions but not into the same one |
| Mock mode      | When `OPENCLAW_RUNTIME_MOCK=1` the adapter skips the spawn and emits a canned three-token reply, useful for CI and for hand-developing voice on a machine without OpenClaw installed |

> **Why we copy pet-claw's helper-process pattern instead of importing
> `agentCommand` directly into the bus**: OpenClaw's runtime pulls in a
> *lot* of dependencies (transformers, fs cache, etc.) and is intentionally
> long-lived; running it in the same Node process as the bus would couple
> their crash surface and memory footprint. The helper is a couple hundred
> lines, single-purpose, and trivially restartable — we're following the
> exact precedent that already exists.

## 5. voice-service-node: what changes

### 5.1 Removed
- `defaults.llm` config block (and the entire `openai_internal` provider).
- The LLM/agent/persona env vars `LOCAL_AGENT_BACKEND`, `LOCAL_AGENT_MODEL`,
  `LOCAL_AGENT_BASE_URL`, `LOCAL_AGENT_API_KEY`, `PET_CLAW_PERSONA_MODEL`.
  pet-manager stops setting them.

### 5.2 Added
- `VOICE_BUS_URL` (default `http://127.0.0.1:8181`) — where to POST inject.
- `VOICE_AGENT_ID` — set by pet-manager, equals `selected_agent_id`. Voice
  refuses to start if empty (matches "no agent → manager-level failure").
- `VOICE_SESSION_ID` (default `auto`) — set by pet-manager when the user
  picks a specific session in the UI.

### 5.3 `worker_entry.mjs` rewrite

Today: the SDK's worker uses `defaults.llm` to talk to mimo internally.

After: the worker subscribes to the LiveKit room as a participant, runs
the volc STT, and on each STT result:

```ts
const sse = await fetch(`${VOICE_BUS_URL}/agent/inject`, {
  method: "POST",
  body: JSON.stringify({
    agentId: VOICE_AGENT_ID,
    sessionId: VOICE_SESSION_ID,
    text: stt.text,
    metadata: { source: "voice", stt: { provider: "volc", ... } },
  }),
});
for await (const evt of parseSSE(sse)) {
  if (evt.kind === "token") tts.feed(evt.text);
  if (evt.kind === "done") tts.flush();
  if (evt.kind === "error") tts.feed(`抱歉，agent 暂时无法响应：${evt.message}`);
}
```

The PetAgent LiveKit Agent SDK's role here shrinks: it provides token API,
room participation, STT/TTS plumbing. **It no longer drives the LLM.**

### 5.4 `roles.yaml` after the diet

```yaml
version: 1
livekit:
  url: ${LIVEKIT_URL:-}
  api_key: ${LIVEKIT_API_KEY:-<YOUR_LIVEKIT_API_KEY>}
  api_secret: ${LIVEKIT_API_SECRET:-<YOUR_LIVEKIT_API_SECRET>}

agent:
  name_prefix: petagent-agent

providers:
  volc_main:
    type: volcengine
    app_id: ${VOLC_APP_ID:-<YOUR_VOLC_APP_ID>}
    access_token: ${VOLC_ACCESS_TOKEN:-...}

defaults:
  stt:
    provider: volc_main
    options: { resource_id: volc.bigasr.sauc.duration, sample_rate: 16000 }
  tts:
    provider: volc_main
    options: { cluster: volcano_tts, voice: BV700_streaming, sample_rate: 24000, streaming: true }

voice_agents:
  - name: bus-frontend
    # No llm.system_prompt: prompts live with the agent (Claude/Codex/OpenClaw),
    # not with us. We're an audio terminal.
```

## 6. claw-pet-manager: what changes

### 6.1 Rust (`src-tauri/src/lib.rs`)

| Change | Detail |
|---|---|
| Remove `build_voice_agent_env_pairs` LLM env (`LOCAL_AGENT_BASE_URL`, `LOCAL_AGENT_MODEL`, `PET_CLAW_PERSONA_MODEL`) | They're misleading (port 18789 is pet-claw WS, not an OpenAI endpoint anyway) |
| Add `VOICE_BUS_URL` / `VOICE_AGENT_ID` / `VOICE_SESSION_ID` env when spawning voice-service | The first comes from a constant; the latter two from `profile.selected_agent_id` and `profile.voice_session_id` |
| Add bus sidecar lifecycle | The bus runs in the existing bridge Node process — no new sidecar binary; just a new `agent-session-bus` package mounted on port 8181 |
| `ensure_voice_runtime` precondition | Refuse to start if `selected_agent` is not `ready` (returns a clear error mode for the UI) |

### 6.2 UI (`ref/src/`)

| Change | Detail |
|---|---|
| Voice button enable state | Board-audio enable stays clickable while Bridge / voice-service are still being checked, performs `ensure_bridge_runtime` then `ensure_voice_runtime`, and only disables when no agent is selected or the selected agent is known unavailable |
| Voice settings panel | `当前语音 agent: <selected_agent_label>` (read-only — to switch, change selected_agent in the agent picker) |
| Session dropdown | List sessions from `GET /agent/sessions`, default = "最近 active session"; "新开 session" as the last option |
| Live status | A small chip showing `voice → claude-code → /Users/x/proj <session 30 min ago>` so the user knows where their voice is going |
| Board-audio target | When sending `audio_bridge`, the UI uses the current online board id resolved from availability first, then falls back to the persisted binding id for older bindings |
| Board voice config | Pet Manager stores `{enabled, trigger}` where `trigger` is `top_button.hold` or `encoder_button.hold`; it never captures board voice itself. Applying the config sends `audio_bridge` through USB serial when connected and through MQTT as a fallback. |

### 6.3 Board runtime (`board-runtime`)

| Component | Detail |
|---|---|
| `board-server` | Parses `audio_bridge` control commands from USB `control/command` and MQTT, writes `.audio-bridge-config` plus `.voice-button-config`, and starts/stops `board-audio-bridge.sh`. |
| `board-rotary-input` | Reads `.voice-button-config`; while board voice is enabled, the selected physical button hold becomes push-to-talk and normal page/reset actions are suppressed only for that hold. |
| `board-audio-bridge.sh` | Starts the board playback listener and uses board `arecord`/`aplay` with UDP `nc`; `ptt-start` captures from the board microphone to the desktop port, and `ptt-stop` stops capture. |
| Default button | `top_button.hold`; new users see the token-consumption/stats negative screen in the client guide, and the top red button still short-presses between `main` and `stats` when voice is not actively assigned/enabled. |

## 7. Failure modes (and what we do)

| Condition | Behavior |
|---|---|
| `selected_agent` empty | Manager UI banner "未选择 agent，请先在 Agent 面板选一个"; voice button hidden |
| `selected_agent.ready === false` | Manager UI banner "agent <name> 未安装/未配置"; voice button disabled with tooltip |
| Bus port 8181 not bound | voice-service polls bus on startup, waits up to 5 s; after that surface "本地服务未启动" |
| `inject` returns `AGENT_UNAVAILABLE` | TTS plays "抱歉，<agent name> 暂时不可用" once; voice session goes idle until next mic event |
| `inject` SSE drops mid-stream | Treat as `{kind:"error",code:"STREAM_INTERRUPTED"}`; TTS plays "抱歉，连接中断"; the agent run on the agent side keeps going (we never kill the agent process) |
| OpenClaw selected but npm pkg not installed | `isAvailable()` returns `{ready:false, reason:"openclaw runtime module not found"}`; voice button disabled with that tooltip. WS gateway being up is not enough — write path needs the npm package. |
| OpenClaw helper crashes mid-run | Adapter emits `{kind:"error", code:"RUNTIME_HELPER_CRASHED"}` and respawns next call; the agent's sid is preserved (`sessions.json` is on disk). |
| Mic captures partial / noise → STT empty | voice-service short-circuits; no inject is sent |

## 8. The end-to-end test that proves "session integrity"

Pre-conditions:

1. User has Claude Code installed (`claude --version` works).
2. In a terminal in `/Users/x/proj`, user runs `claude` and says: "请帮我把
   `src/main.rs` 重构成 hooks 风格，先列出现有结构。" Claude responds with a
   plan.
3. pet-manager shows: agent=`claude-code`, status=`ready`, voice button
   enabled, session dropdown highlights the just-touched session.

Steps:

1. User enables board voice in Pet Manager, chooses the physical voice button,
   and applies the config to the board over USB.
2. User holds the selected physical board button and speaks: "那直接帮我改吧。"
3. Expected:
   - voice-service STT yields `"那直接帮我改吧。"`.
   - Bus dispatches to ClaudeCodeAdapter → `claude -p "那直接帮我改吧。"
     --resume <same sid> --output-format stream-json`.
   - Claude continues *the same conversation*: it acknowledges the plan,
     edits files via tool calls, says "改好了".
   - Voice TTS plays "改好了" through the chosen output (Mac speaker / 板子
     audio_bridge).
   - Back in the terminal, the same `claude` REPL shows the user's voice
     turn and Claude's response in the conversation log (because both
     reference the same `<sid>.jsonl`).

This is the test that gates merging. If the same `<sid>.jsonl` doesn't get
appended-to, the architecture has not landed yet.

## 9. Out of scope (for the first ship)

- Wake-word / always-listening mode — voice button is push-to-talk.
- Voice command for switching agents ("切到 codex") — explicitly disallowed
  per architecture review (avoids accidental leaks across agents/projects).
- Multi-user / multi-tenant. The bus is local-only; one user per machine.
- Remote OpenClaw (running on a different host than pet-manager). Voice
  expects `agent-runtime` to be importable on the same machine the bus
  runs on. Cross-host OpenClaw will need a thin RPC shim later, but it's
  not on the critical path.

## 10. Roll-out checklist

- [ ] (this file) Architecture written and reviewed
- [ ] Bus skeleton + adapter interface in `claw-pet-manager` bridge
- [ ] ClaudeCodeAdapter: `~/.claude/projects` schema verified against current `claude` CLI
- [ ] CodexAdapter: `--output-format stream-json` verified against current Codex CLI
- [ ] OpenClawAdapter: helper-process pattern lifted from pet-claw,
      `agentCommand()` exercised end-to-end on a machine with `openclaw`
      npm installed
- [ ] voice-service-node refactored, `roles.yaml` slimmed
- [ ] pet-manager Rust env vars switched
- [ ] pet-manager UI: enable-state + session dropdown + status chip
- [ ] §8 E2E test passes for `claude-code` and `codex`
- [ ] §8 E2E test passes for `openclaw` (no external dependency — if
      `openclaw` npm pkg is on the machine, this works today)
- [ ] PRs split as: voice-bus (pet-manager) · vsn refactor (voice-service-node) · docs+board followups
