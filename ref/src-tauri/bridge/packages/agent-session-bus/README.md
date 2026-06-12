# agent-session-bus

A small in-process Node.js HTTP server that voice-service-node talks to so a
single STT utterance can be turned into "next user turn" inside the user's
currently-selected coding-agent session.

This package is the implementation of `docs/voice-architecture.md` §3.

## Why this exists

Without this bus, voice-service-node would have to know how to talk to Claude
Code, to Codex, and to OpenClaw — each of which has a totally different write
path:

| agent          | write path                                                    |
|----------------|---------------------------------------------------------------|
| `claude-code`  | spawn `claude -p ... --resume <sid> --output-format stream-json` |
| `codex`        | resume Desktop threads via `codex app-server --listen stdio://`; new sessions use `codex exec --json` |
| `openclaw`     | spawn helper → `import('openclaw/dist/plugin-sdk/agent-runtime').agentCommand({...})` |

Centralising the "find the user's latest session, pipe text in, stream tokens
out" logic here keeps voice-service-node a pure audio frontend, and keeps each
agent's transport peculiarities behind a single `AgentAdapter` interface.

## Wire shape

```
voice-service-node                 agent-session-bus              <agent>
     │                                   │                          │
     │ POST /agent/inject                │                          │
     │ { agentId, sessionId, text }      │                          │
     │──────────────────────────────────>│                          │
     │                                   │ resolveSession(...)      │
     │                                   │ inject(sid, text)        │
     │                                   │─────────────────────────>│
     │                                   │                          │
     │ <─── SSE: token, tool, done ──────│ <─── streamed events ────│
```

### `POST /agent/inject` (SSE response)

Body:
```jsonc
{
  "agentId": "claude-code" | "codex" | "openclaw",
  "sessionId": "auto" | "<adapter-specific id>",
  "text": "<STT utterance>",
  "metadata": { "source": "voice", "stt": {...}, "locale": "zh-CN" }
}
```

Response: an SSE stream where each event is one of
```
event: token
data: {"text": "..."}

event: tool
data: {"name": "Edit", "phase": "start" | "end", "input": {...}, "ok": true}

event: done
data: {"sessionId": "...", "tokens": 42, "stopReason": "end_turn"}

event: error
data: {"code": "AGENT_UNAVAILABLE", "message": "..."}
```

### `GET /agent/status`

Returns availability info for every registered adapter:
```jsonc
{
  "adapters": [
    { "agentId": "claude-code", "ready": true,  "detail": null },
    { "agentId": "codex",       "ready": true,  "detail": null },
    { "agentId": "openclaw",    "ready": false, "detail": "openclaw npm pkg not found" }
  ]
}
```

### `GET /agent/sessions?agentId=<id>&limit=<n>`

Returns the most-recent sessions for the given agent (used by the UI session
dropdown).

### `POST /agent/cancel`

Body: `{ "runId": "..." }` — cancels an in-flight `inject` SSE. The agent run
itself keeps going; only our pipe is closed.

## Embedding it

The bus is **not** a standalone sidecar. It is mounted inside the existing
`clawd-backend-service` Node sidecar process. See `src/index.js` for the
factory:

```js
const { createAgentSessionBus } = require("agent-session-bus");
const { ClaudeCodeAdapter } = require("agent-session-bus/adapters/claude-code");

const bus = createAgentSessionBus({
  port: 8181,
  log,
  adapters: [
    new ClaudeCodeAdapter({ /* ... */ }),
    // codex, openclaw, ...
  ],
});

await bus.start();
// ...
await bus.stop();
```

## Adapter interface

See `src/adapters/base.js` for the full `AgentAdapter` interface. New agents
plug in by extending `BaseAdapter` and implementing four methods:
`isAvailable`, `listSessions`, `openNew`, `inject`. (`resolveActive` has a
default "newest in `listSessions()`" implementation that matches the
"永远续最近" rule from the architecture doc.)
