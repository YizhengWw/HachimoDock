# @hachimodock/dsh-bridge

A DeepSeek Harness (DSH) Host plugin that exposes a **localhost-only HTTP/SSE
bridge** for HachimoDock. It lets the HachimoDock PC bridge and the ESP32-P4
display:

- list the DSH sessions the running Host can observe,
- send a voice/caption prompt into a live session and watch its reply stream,
- mirror DSH permission prompts to the device and apply the device's one-shot
  `allow` / `deny` decision.

This is the DSH side of `docs/dsh-integration-contract.md` (R1 session/voice,
R5 approval-on-screen) in the HachimoDock repository. It is a **v0 bridge**:
the wire protocol is version 1, localhost has no authentication yet, and only
live (in-memory) sessions are supported.

No npm dependencies: the plugin is plain CommonJS and uses Node built-ins only.

## Architecture

```text
DSH Host process
┌────────────────────────────────────────────────────────────────┐
│ Cordis composition                                             │
│                                                                │
│  index.js ── creates ──► dsh-bridge.js ── uses ──► server.js   │
│  (Cordis plugin)         (DSH ctx boundary)       (HTTP + SSE) │
│       │                        │                        │      │
│       │ ctx.effect             │ ctx.sessions.list()    │      │
│       │ ctx.on('approval/      │ ctx.agents.get()       │      │
│       │   request', …)         │ agent.followup()       │      │
│       │ ctx.on('session/       │ agent.whenIdle()       │      │
│       │   event', …)           │ session/event          │      │
│       ▼                        ▼                        ▼      │
│  listener lifetime         adapters               127.0.0.1:17892
└────────────────────────────────────────────────────────────────┘
                                              │ HTTP / SSE
                                              ▼
                                  HachimoDock PC bridge
                                              │ USB
                                              ▼
                                       ESP32-P4 display
```

- **`index.js`** is the Cordis plugin entry. It exports the plugin shape
  (`name`, `inject: ['sessions', 'agents']`, `apply`) and owns the DSH server
  lifetime through `ctx.effect`: the bridge starts when both services are
  available and closes on plugin disposal.
- **`dsh-bridge.js`** is the only module that touches a DSH `ctx`. It turns
  Cordis surfaces into two plain adapter functions (`listSessions`,
  `inject`) plus event listeners, and implements the `approval/request`
  answerer.
- **`server.js`** is transport and state only: it knows no Cordis and no DSH.
  `test/` runs the whole wire contract against it with fake adapters.

The split keeps the security-sensitive HTTP/SSE surface testable with
`node --test` and no DSH process.

## Endpoints

Base URL: `http://127.0.0.1:17892` (port configurable, see
[Configuration](#configuration)). The server always binds `127.0.0.1` and
rejects any non-loopback peer.

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/health` | Liveness and counts |
| `GET` | `/sessions` | Live session summaries |
| `POST` | `/inject` | Deliver a prompt; streams its activity as SSE |
| `GET` | `/events?after=<seq>` | SSE feed of approvals and compact session events |
| `POST` | `/approval/decision` | Apply a one-shot device decision |

All JSON bodies are limited to 64 KB; malformed JSON answers `400
{"ok":false,"error":"invalid-json"}`.

### `GET /health`

```json
{"ok":true,"version":1,"sessions":2,"pendingApprovals":0}
```

### `GET /sessions`

Live sessions only (cold/archived sessions are not resumed by this plugin):

```json
{"sessions":[{
  "id":"session-...",
  "title":"Fix the firmware build",
  "cwd":"/path/to/workspace",
  "lastModified":1790000000000,
  "status":"idle|running",
  "agentId":"deepseek-harness"
}]}
```

`title` is `null` until a title event has been observed (or the optional
`sessionTitle` / `sessionProjections` service is mounted). `lastModified` is
the last observed session-event time in epoch milliseconds.

### `POST /inject`

Request: `{"sessionId":"auto","text":"用户语音文本"}`. `sessionId` `auto` (or
omitted) selects the most recently active live session. Response is
`text/event-stream`:

```text
event: token
data: {"text":"..."}

event: tool
data: {"name":"bash","phase":"start|end","ok":true}

event: done
data: {"sessionId":"session-...","tokens":42}

event: error
data: {"code":"session-not-found|agent-not-live|inject-failed","message":"..."}
```

`token` frames mirror live assistant text deltas; `tokens` in `done` is the
sum of `usage.outputTokens` observed for the injected work, falling back to the
number of mirrored text characters when the provider reports no usage. Invalid
requests are rejected with `400` before the stream opens.

### `GET /events?after=<seq>`

SSE feed (protocol version 1). Frames use SSE `id:` as the event cursor and
mirror the shape in their `data:`:

```text
id: 1
event: approval
data: {"seq":1,"type":"approval","requestId":"req-...","agentId":"deepseek-harness","sessionId":"session-...","tool":"bash","reason":"...","displayReason":"...","expiresAtMs":1790000000000}
```

| `event:` | `data.type` | When |
|---|---|---|
| `approval` | `approval` | Permission prompt pending a `/approval/decision` |
| `approval-asked` | `approval-asked` | DSH audit edge (`approval/asked`), includes `approvalId` |
| `approval-resolved` | `approval-resolved` | Outcome known; includes `approvalId`, `outcome`, and `requestId` when correlated |
| `turn` | `turn` | `turn/start` / `turn/end` (`phase`, `turn`) |
| `tool` | `tool` | `tool/call` / `tool/result` (`phase`, `name`, `callId`, `ok`) |
| `session` | `session` | Session summary on create/status change, or `{"id":..., "removed":true}` on disposal |
| `session-title` | `session-title` | Latest title text for a session |

`after` is an exclusive cursor: frames with `seq > after` are replayed from a
**bounded** 512-event buffer, then live frames follow. Omit `after` to receive
live frames only (recommended for a first connection, so stale approval
prompts are not replayed). A cursor older than the retained window resumes at
the oldest retained event; consumers should treat a `seq` jump as a hint to
refresh `GET /sessions`.

### `POST /approval/decision`

```json
{"requestId":"req-1","decision":"allow"}
```

```json
{"ok":true,"requestId":"req-1","accepted":true}
```

- `allow` closes the DSH request as `allowed-once`.
- `deny` closes it as `rejected`.
- Each `requestId` is consumed exactly once. A repeated decision answers
  `409 {"ok":false,"requestId":"req-1","error":"already-decided"}`; an
  unknown or expired id answers
  `409 {"ok":false,"requestId":"req-1","error":"unknown-or-expired"}`.
  Treat both as terminal — do not retry a decision.
- No decision within the approval timeout closes the request as `unavailable`
  (fail closed), exactly like an absent answerer.

## Approval semantics

The plugin registers a Cordis `approval/request` waterfall listener:

```js
ctx.on('approval/request', (request, next) => handleApprovalRequest(bridge, request, next))
```

For each request it generates a `requestId` (not the DSH audit id), broadcasts
`approval` on `/events`, waits for `POST /approval/decision`, then answers:

| Situation | Answerer outcome |
|---|---|
| device `allow` | `allowed-once` |
| device `deny` | `rejected` |
| timeout (default 30 s) | `unavailable` |
| DSH request signal aborted | `unavailable` |
| no HachimoDock `/events` subscriber attached | `unavailable` (default) |

The vocabulary, one-shot semantics, and fail-closed default come from
`@deepseek-ai/dsh-user-approval`; the plugin deliberately never calls
`ctx.approval.request` — it is an answerer, not a caller.

> ### ⚠️ Coexistence with the resident DSH approval UI
>
> DSH's Waterfall stops at the first listener that returns an outcome. A
> bridge listener registered at plugin mount precedes answerers that the
> Desktop/Web UI registers later through the Remote event channel, so while
> this plugin is mounted it acts as the **terminal** approval answerer: the
> in-app approval panel is not consulted. That is the intended R5 behavior
> when HachimoDock is the approval screen, but it means:
>
> - if no HachimoDock bridge is connected, prompts fail closed after the
>   timeout instead of reaching the desktop panel (default contract behavior);
> - the desktop panel is bypassed for every prompt while the plugin is loaded.
>
> Set `HACHIMODOCK_DSH_APPROVAL_FALLBACK=delegate` (or bundle config
> `approvalFallback: delegate`) to call `next()` instead of answering when no
> `/events` subscriber is attached. Connected HachimoDock still answers
> terminals, so the device remains authoritative while attached. **Verify the
> desired policy on a real DSH Desktop profile before shipping** (see
> [Verified vs. unverified](#verified-vs-unverified)).

## Event forwarding

`ctx.on('session/event', (session, event) => …)` forwards only compact
control-plane data, never transcript content:

- `turn/start`, `turn/end` → `turn` events
- `tool/call`, `tool/result` → `tool` events with `name`, `callId`, `ok`
  (tool arguments and results are not mirrored)
- `session/title` → `session-title` events and the session summary title
- `approval/asked`, `approval/decided` → `approval-asked` /
  `approval-resolved` audit events
- `session/created`, `session/disposed`, `agent/status` → `session` summaries

Field lengths are clamped (titles/reasons/tool names), and the replay buffer is
bounded, so a long session cannot grow the bridge's memory without bound.

## Configuration

Bundle config (from `cordis.patch.yml`) or environment variables. Environment
wins over config; config wins over the defaults.

| Setting | Env | Default | Meaning |
|---|---|---|---|
| `port` | `HACHIMODOCK_DSH_PORT` | `17892` | Loopback port |
| `approvalTimeoutMs` | `HACHIMODOCK_DSH_APPROVAL_TIMEOUT_MS` | `30000` | Fail-closed approval wait |
| `approvalFallback` | `HACHIMODOCK_DSH_APPROVAL_FALLBACK` | `unavailable` in the package; Desktop bundle sets `delegate` | `delegate` keeps the normal DSH approval UI when no HachimoDock `/events` subscriber is attached; `unavailable` is the strict headless fail-closed stance |

The server always binds `127.0.0.1`; there is no option to bind elsewhere.
Additional internal option: `injectTimeoutMs` / `HACHIMODOCK_DSH_INJECT_TIMEOUT_MS`
(default 10 minutes) bounds one `/inject` stream.

## Client half is part of the same bundle

The package has both halves:

- `index.js` + `dsh-bridge.js` + `server.js` — **Host** (Plugins page row `dsh-bridge`);
- `client.js` — **Client/Web UI** half, declared by `dsh.client` in `package.json`.

The DSH Plugins page therefore shows **one** row, `dsh-bridge`; there is no
separate `client` row. After a DSH restart the Web UI loads `client.js` and it
opens `/events?role=client` from the renderer. On macOS this is visible as a
DeepSeek Harness Helper (Renderer) TCP connection to `127.0.0.1:17892`.

If that renderer connection is absent, the client half did not load; restart
DSH and re-select/reinstall the bundle.

## Installation

> **Do not install this package into a running DSH profile as part of
> development.** The commands below are for testers; they modify the target
> profile (`~/.dsh/profiles/desktop`). Restart DSH after installing on a
> startup-only profile (Desktop has no HMR).

The package declares `dsh.bundle.patch: ./cordis.patch.yml`, so it is a DSH
**bundle**: installing it mounts one Host row (`hachimodock-bridge`) that
starts the bridge.

### Option A — DSH Plugin Manager (recommended)

Use the Web sidebar **Plugins** page, or ask an agent with the
`plugin_manager` tool (Creator mode) to install this package from its absolute
directory:

```text
plugin_manager({
  action: "install_bundle",
  target: "/absolute/path/to/integrations/dsh-hachimodock"
})
```

`plugin_manager` performs the profile `package.json` / `cordis.patch.yml` /
bundle-selection edits itself. Do not hand-edit those files.

### Critical: bundle order on Desktop/Web profiles

The bridge's `approval/request` answerer must register **before** the Desktop/Web
approval UI, otherwise the native UI answers first and the device never sees the
prompt. `plugin_manager` appends a bundle at the end of
`dsh.profile.bundles`, so after installing, ensure this order:

```yaml
bundles:
  - "@deepseek-ai/dsh-base"
  - "@hachimodock/dsh-bridge"
  - "@deepseek-ai/dsh-web-app"
  # remaining bundles
```

Ahead of `dsh-web-app`, the bridge answers first when a HachimoDock `/events`
subscriber is attached; with no subscriber it uses `approvalFallback: delegate`
and the normal DSH UI remains authoritative.

### Option B — `dsh plugin` CLI

`dsh plugin` only performs package management; select the bundle after the
package installs.

```bash
# Desktop profile must already exist (open DSH Desktop once, then quit it).
dsh plugin --profile desktop add /absolute/path/to/integrations/dsh-hachimodock
```

Then deselect/reselect or restart the profile so the bundle loads, e.g. from
the Web Plugins page. On a source checkout, `dsh plugin --profile <p> add` also
accepts the `link:` form for a workspace package.

### Profiles other than Desktop

The same install works for any DSH profile (`web`, `headless`, …). In a
headless/acp profile the bridge's answerer is the only approval channel; keep
`approvalFallback: unavailable` there so missing devices fail closed.

### Smoke-test without installing

```bash
cd integrations/dsh-hachimodock
node --test            # 51 tests, no DSH process and no profile changes

# or mount a bridge with fake services (useful when wiring the PC adapter):
node -e "
const { BridgeServer } = require('./server')
const server = new BridgeServer({ port: 17892, listSessions: () => [] })
server.listen().then(() => console.log(server.url()))
"
curl -s http://127.0.0.1:17892/health
```

## Verified vs. unverified

Verified in this checkout (Node 26, `node --test`, 51 tests):

- HTTP contract: health, sessions, inject SSE (`token`/`tool`/`done`/`error`),
  events SSE with `after` replay, decision `200`/`409`, body limit, loopback
  binding, routing errors.
- DSH boundary with fake services: answerer mapping (`allowed-once`,
  `rejected`, timeout/abort/no-bridge `unavailable`), `/events` forwarding of
  turn/tool/title/approval audit events, session summaries, prompt injection
  (`followup` user message; live token deltas; tool pairs; output tokens).
- Real **Cordis 4.0.5** from the installed Desktop `app.asar` (read-only
  extraction), with fake `sessions`/`agents` services: CommonJS plugin import
  through the Loader's `import()` path, deferred service injection, plugin
  mount/dispose (`ctx.effect` closes the HTTP server), and the full approval
  loop (waterfall → `/events` approval frame → decision → `allowed-once`).

Not verified against a **live DSH Host profile** — these need a real-device
test, and are the top integration risks:

1. **Answerer ordering vs. the Desktop approval UI.** The plugin loads at
   boot; the Web/Desktop UI registers its Remote approval answerer later, so
   the bridge's terminal listener can shadow the in-app approval panel. The
   real ordering (and whether a second terminal answerer produces a warning)
   must be checked on a Desktop profile, especially with
   `approvalFallback=delegate`.
2. **Approval abort vocabulary.** `@deepseek-ai/dsh-user-approval` races the
   answerer against the request signal and reports `cancelled` on abort. The
   bridge promptly answers `unavailable` on abort, but the service's race may
   make the final audited outcome `cancelled` instead; confirm the audit
   events and UI wording.
3. **`agent.whenIdle()` under queued work.** The bridge calls `followup()`
   then awaits `whenIdle()` so `done` reflects the whole delivered turn.
   Confirm on a live loop that a queued prompt and its follow-up turn settle
   as expected (and that an idle agent does not emit a stale `done`).
4. **Session/event names and stream chunks.** `turn/start`, `turn/end`,
   `tool/call`, `tool/result`, `assistant/message`, `session/title`,
   `approval/asked`, `approval/decided`, and `agent/assistant-stream` chunks
   (`{type:"text-delta", text}`) were read from the shipped
   `dsh-session`/`dsh-agent-loop` builds; validate they arrive as expected on a
   live turn, and that `usage.outputTokens` is populated by the configured
   provider (the bridge falls back to character count).
5. **Title projection availability.** `ctx.get('sessionTitle')` /
   `ctx.get('sessionProjections')` are optional; if the profile does not mount
   them, a session shows `title: null` until its next title event.
6. **Cold sessions.** Only `ctx.sessions.list()` (live sessions) is used; a
   session that is not resident is not listed or injectable.
7. **CJS loading in the real Loader.** Simulated with Node's `import()` +
   `unwrapExports`; confirm the profile row imports `@hachimodock/dsh-bridge`
   and reports the correct plugin name in Loader diagnostics.

Other known limitations (v0): no authentication/pairing token on the loopback
socket; no conversation history endpoint; subagent/child sessions appear in
`/sessions` because the store lists them; a decision races a 30 s timeout, so
a late device tap answers `409`; no device-side dismissal when a prompt is
answered elsewhere (the `approval-resolved` event is the hook for a follow-up).

## Testing

```bash
cd integrations/dsh-hachimodock
node --test
```

The suite uses `node:test` + `node:assert/strict` only. `test/helpers.js`
provides a fake Cordis context (`ctx.on`, `ctx.sessions`, `ctx.agents`,
`ctx.get`, logger), fake sessions/agents, and an SSE reader built on `fetch`.

## License

MIT (same as the embedding HachimoDock repository).
