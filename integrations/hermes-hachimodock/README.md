# HachimoDock Hermes bridge

Dependency-free M1 integration for [Hermes Agent](https://hermes-agent.nousresearch.com):
the desktop pet follows the active Hermes session by **read-only polling of
`~/.hermes/state.db`** (SQLite/WAL) and reports the mapped state to the local
HachimoDock bridge's `POST /state`. Hermes itself is never modified — no hook,
no config write, no lock on its database — and no firmware change is needed.

## Layout

| File | Role |
| --- | --- |
| `index.js` | Entry: resolve Hermes home + bridge port, start the monitor |
| `hermes-monitor.js` | Read-only poller; increments a persisted `messages.id` watermark |
| `state-map.js` | Message row / context → one of the 11 deskpet states (pure) |
| `identity.js` | `session_id` + Hermes `source` + hostname prefix → device identity |

## Install & run

Requires Node ≥ 22.5 (`node:sqlite`). From this directory:

```sh
node index.js            # poll every 1s, POST to the bridge's /state
# or with explicit endpoints:
HERMES_HOME=~/.hermes HACHIMODOCK_STATE_PORT=23333 node index.js
```

- Bridge port is read from `~/.clawd/runtime.json` (`port` / `statePort` /
  `bridgePort` / `http.port`); when that file is absent the bridge probes
  `127.0.0.1:23333~23337` with `GET /state`.
- The followed session is the one with the newest message row. Pin it by
  writing `{"sessionId": "..."}` to `~/.clawd/hermes-hachimodock-lock.json`
  (set `HERMES_LOCK_FILE` to move the file).
- Watermark and paths are configurable via `HERMES_STATE_DB`,
  `HERMES_WATERMARK_FILE`, `HERMES_POLL_MS`; all paths default under
  `~/.hermes` / `~/.clawd` with `HERMES_HOME` / `CLAWD_HOME` overrides.

## State mapping

| Hermes signal | Reported state |
| --- | --- |
| new `role=user` row | `working` |
| `assistant` + `finish_reason=tool_calls` | `thinking` |
| `role=tool` row | `working` |
| tool result content with `error` / `is_error` / `exit_code != 0` | `error` |
| `assistant` streaming (no `finish_reason`, content present) | `speaking` |
| `assistant` + `finish_reason=stop` / `length` | `attention` |
| followed session has `ended_at` | `sleeping` |
| unanswered `clarify` tool call | `notification` |
| active `async_delegations` row | `juggling` |
| `compression_locks` row / new `_compressed_summary` row | `sweeping` |
| active `worker_executions` row | `carrying` |
| nothing new | `idle` |

The vocabulary is strictly these 11 states; multi-session agents get no extra
semantics (the bridge folds them into pet motion states anyway).

## Checks

`npm test` (or `node --test`) runs the pure mapping tests plus an end-to-end
poll against a temporary `state.db` and a mock `/state` endpoint. No Hermes
install, network call or physical device is required.

## Troubleshooting

- `node:sqlite unavailable on this Node build` → upgrade Node to ≥ 22.5.
- `POST /state failed` logs → the bridge sidecar is not running; start the
  desktop app, or point `HACHIMODOCK_STATE_PORT` at the right loopback port.
- Stale state after restart → delete `~/.clawd/hermes-hachimodock-watermark.json`
  to re-seed the watermark from the newest message.
- Hermes upgraded and renamed a column → the poller fails closed (logs the
  error, keeps the last watermark); it never guesses at Hermes internals.

## Notes

- License: MIT, matching `integrations/dsh-hachimodock` (the repo-level
  license question in issue #28 is answered by that precedent; the maintainer
  can retarget it if they prefer otherwise).
- Scope is M1 (status following) only. M2 voice injection, M3 permission
  approval and M4 telemetry are separate follow-ups.
