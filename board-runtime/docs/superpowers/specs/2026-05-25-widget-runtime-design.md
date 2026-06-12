# Widget Runtime System — design spec

## Goal

Let `petAgent-ui-generator` skill emit a fully working widget. Install → push to RPi → device actually runs the widget (button transitions, periodic tick, counter changes, multi-page, optional HTTP fetch / on-device file read). No per-widget code shipped from the LLM.

## Architecture

```
+----- claw-pet-manager (Mac) -----+
|  ComponentCenter UI              |
|  petAgent-ui-generator skill     |
+-------------+--------------------+
           | SSH/sudo write (install_clawpkg_over_ssh)
           v
+--- /opt/board-runtime/ (Pi) ---+
|  widgets/<id>/                  ← one dir per widget, all static files
|    ├── component.json
|    ├── negative-screen.json    (initial dashboard slot values)
|    ├── buttons.json            (action ↔ hardware control mapping, override-able)
|    ├── runtime/widget.json     (NEW: state machine + tick + fetchers + readers)
|    ├── assets/
|    └── share.json
|  .active-widget                 ← one line: active widget id
|                                 |
|  +-- board-widget-runtime ---+  ← NEW Python asyncio daemon
|  |  watch .active-widget    |     - load widgets/<id>/widget.json + buttons.json
|  |  watch .current-event    |     - validate (JSON Schema)
|  |  interpret state machine|     - dispatch input events → transitions
|  |  schedule tick / fetch  |     - apply ticks / fetchers / readers
|  |  emit .stats-display    |     - re-render dashboard on var/state/page change
|  +-----------------------------+
|                                 |
|  fb-display.sh                  ← unchanged. Polls .stats-display every 2s
|     ↓                           |
|  fb-stats-renderer.py           ← already supports COMPONENT_DASHBOARD_V1 + CJK
|     ↓                           |
|  /dev/fb1 (320×240 LCD)         |
|                                 |
|  board-rotary-input             ← unchanged. Writes to .current-event
|  board-touch-input              ← unchanged. Writes to .current-event
+---------------------------------+
```

**Key decoupling decisions:**
- New process, not embedded in board-server (C). Faster iteration, smaller blast radius.
- Existing input + output binaries unchanged. Only new component: interpreter.
- `.active-widget` is the mode switch; only one widget active at a time (matching current `.screen-page` semantics).
- skill emits declarative JSON only. No code generation. No `eval`. Turing-incomplete grammar.

## .clawpkg structure (extended)

```
<id>/
├── component.json        # unchanged (id/name/version/author/description)
├── negative-screen.json  # unchanged (initial COMPONENT_DASHBOARD_V1 slot values)
├── buttons.json          # action ↔ hardware control mapping (override-able by client)
├── runtime/
│   └── widget.json       # NEW: state machine spec
├── assets/.keep          # unchanged
└── share.json            # unchanged
```

## widget.json grammar

```json
{
  "schema_version": 1,

  "vars": {
    "elapsed_s": { "type": "int", "init": 0 },
    "target_s":  { "type": "int", "init": 1500 }
  },

  "states": ["idle", "running", "paused"],
  "initial_state": "idle",

  "pages": [
    { "id": "main",   "label": "计时" },
    { "id": "today",  "label": "今日" }
  ],
  "initial_page": "main",

  "transitions": [
    { "from": "idle",    "on": "timer.start_pause", "to": "running" },
    { "from": "running", "on": "timer.start_pause", "to": "paused"  },
    { "from": "paused",  "on": "timer.start_pause", "to": "running" },
    { "from": "*",       "on": "timer.reset",       "to": "idle", "set": { "elapsed_s": 0 } },
    { "from": "idle",    "on": "timer.bump_up",     "inc": { "target_s":  60 } },
    { "from": "idle",    "on": "timer.bump_down",   "inc": { "target_s": -60 } },
    { "from": "*",       "on": "nav.next_page",     "page": "next" }
  ],

  "tick": [
    { "every_ms": 1000, "while_state": "running", "inc": { "elapsed_s": 1 } }
  ],

  "fetchers": {
    "github_prs": {
      "url": "https://api.github.com/repos/anthropics/claude-code/pulls?state=open",
      "every_s": 60,
      "parse": "json",
      "json_path": "$.length",
      "into": "pr_count"
    }
  },

  "readers": {
    "token_stats": {
      "path": ".token-stats",
      "every_s": 5,
      "field_pattern": "metric_value=(\\d+)",
      "into": "token_count"
    }
  },

  "dashboard": {
    "title":       "📅 会议计时",
    "eyebrow":     "本场会议",
    "headline":    { "switch_state": { "idle": "未开始", "running": "计时中", "paused": "已暂停" } },
    "metricLabel": "已用时长",
    "metricValue": { "fmt_mmss": "elapsed_s" },
    "metricUnit":  "",
    "badge":       "",
    "note":        "",
    "footer":      { "switch_state": {
                       "idle":    "红钮 开始 · 旋钮 调时长",
                       "running": "红钮 暂停 · 长按 重置",
                       "paused":  "红钮 继续 · 长按 重置" }}
  }
}
```

### Grammar rules

| Top-level key | Required | Semantics |
|---|---|---|
| `schema_version` | yes | int, must be 1 |
| `vars` | no (default `{}`) | `{name: {type, init, source?}}`. type ∈ `int`/`string`. source ∈ `"fetcher.<id>"` or `"reader.<id>"` (auto-update from fetcher/reader). |
| `states` | yes | non-empty array of state ids (kebab-case) |
| `initial_state` | yes | must be in `states` |
| `pages` | no | array of `{id, label?}`. Omit for single-page widgets. |
| `initial_page` | only if `pages` set | must be one of `pages[*].id` |
| `transitions` | no | array of `{from, on, to?, set?, inc?, page?}`. `from` = state id or `"*"`. `on` = action name (declared in buttons.json). |
| `tick` | no | array of `{every_ms, while_state?, set?, inc?}`. `every_ms ≥ 100`. |
| `fetchers` | no | `{id: {url, every_s, parse, json_path, into}}`. `every_s ≥ 30`. `parse ∈ {"json", "text"}`. `json_path` subset: `$`, `$.field`, `$.field.length`. `into` must be a declared var. URL must match runtime allowlist (see Security). |
| `readers` | no | `{id: {path, every_s, parse?, field_pattern?, into}}`. `path` must match whitelisted prefix. `every_s ≥ 1`. |
| `dashboard` | yes | `{slot_name: rule}` for any of the 10 COMPONENT_DASHBOARD_V1 slots. Slots not declared default to empty. |

### Dashboard rendering rules (only these 4 shapes)

| Shape | Meaning |
|---|---|
| `"literal string"` | render verbatim |
| `{ "switch_state": { state_id: "...", ...} }` | pick string by current state |
| `{ "switch_page": { page_id: "...", ...} }` | pick string by current page |
| `{ "fmt_mmss": "var_name" }` / `{ "fmt_hms": "var_name" }` | format int seconds as MM:SS / H:MM:SS |
| `{ "var": "var_name" }` | str/int conversion of variable value |

### What's deliberately excluded

- No conditionals (no `if`/`when`). Use state transitions instead.
- No arithmetic beyond `set` (literal) and `inc` (integer delta).
- No string concatenation in dashboard.
- No write-back to vars from dashboard (no side effects).
- No nested objects beyond what's shown above.

### v2 extensions (NOT v1)

- `transitions[*].when: {var: x, gt: 5}` conditional guards
- `transitions[*].emit: "event_name"` for cross-widget communication
- `dashboard[*]: { "concat": [...] }` for string composition

Each v2 extension must keep: JSON-Schema-validatable, LLM-safe to emit, reads like config not code.

## buttons.json (action ↔ hardware decoupling)

**widget.json transitions reference *action names* (`timer.start_pause`), not hardware events (`button.primary.short_press`).** buttons.json is the only place that does the action↔hardware translation.

```json
[
  { "action": "timer.start_pause", "control": "红色按钮", "event": "button.primary.short_press", "label": "开始/暂停" },
  { "action": "timer.reset",       "control": "红色按钮", "event": "button.primary.long_press",  "label": "重置" },
  { "action": "timer.bump_up",     "control": "旋钮",     "event": "knob.rotate_cw",             "label": "调长" },
  { "action": "timer.bump_down",   "control": "旋钮",     "event": "knob.rotate_ccw",            "label": "调短" },
  { "action": "nav.next_page",     "control": "屏幕区域", "event": "screen.region.tap",          "label": "切页" }
]
```

**Client override:** when user changes "开始/暂停" from 红色按钮 to 旋钮, the entry becomes `{action: "timer.start_pause", control: "旋钮", event: "knob.button.short_press", label: "开始/暂停"}`. widget.json untouched.

**Runtime routing:**

```
hardware event (.current-event line, e.g. {control:"红色按钮", event:"button.primary.short_press"})
  → find buttons.json entry matching (control, event)
  → take entry.action ("timer.start_pause")
  → find widget.json transitions[*] where on == action AND (from == current_state OR from == "*")
  → apply to / set / inc / page
```

## Device-side runtime: board-widget-runtime.py

### Process model

- Single Python 3 asyncio daemon.
- Systemd unit `board-widget-runtime.service` (after `board-runtime.service`, restart=on-failure).
- Added to `start-rpi.sh` startup sequence.

### Main loop

```python
async def main():
    state = WidgetRuntime()
    await asyncio.gather(
        state.watch_active_widget(),   # inotify on .active-widget
        state.watch_input_events(),    # inotify-tail on .current-event
        state.run_ticks(),             # one task per tick spec
        state.run_fetchers(),          # one task per fetcher
        state.run_readers(),           # one task per reader
    )
```

### Lifecycle events

- **Boot**: read `.active-widget`. If empty / file missing → idle (no widget loaded, .stats-display untouched).
- **Active widget change**: cancel all per-widget asyncio tasks, load new widget, validate, restart task graph.
- **Input event**: parse JSON line from `.current-event`, look up action via buttons.json, fire matching transition.
- **Validation failure**: log error, do NOT activate. Previous `.stats-display` content stays (graceful degradation).
- **Runtime error** (fetcher 5xx, reader file missing): log, set var to last-known / default, do NOT crash.

### Output

- After any state / var / page change → recompute all dashboard slots → atomic write `.stats-display` (tmp + rename).
- `fb-display.sh` polls `.stats-display` every 2s → picks up new payload → re-renders.
- Optional optimization: send `SIGUSR1` to fb-display.sh on change (eliminates polling lag).

### Security

- **Fetcher URL allowlist**: runtime config file `/opt/board-runtime/widget-runtime.conf` lists allowed hosts. Default: empty (all fetchers disabled). User edits config to permit specific hosts (`api.github.com`, `api.openweathermap.org`, etc.). Skill can emit URLs but they won't fetch unless host is in allowlist.
- **Fetcher rate limit**: `every_s` minimum 30s enforced at runtime regardless of widget.json claim.
- **Reader path whitelist**: only paths under `/opt/board-runtime/.{stats-display,token-stats,current-speech,screen-page,active-widget,debug-*}` are readable. Hardcoded prefix check.
- **Reader rate limit**: `every_s` minimum 1s.
- **HTTPS only** for fetchers. No HTTP, no file://, no other schemes.
- **No env var access**, no command execution, no socket beyond outbound HTTPS to allowlisted hosts.

## Client-side: install_clawpkg_over_ssh

### New Tauri command (Rust)

```rust
#[derive(Deserialize)] struct InstallSshInput {
    clawpkg_path: String,
    ssh_host: String,        // e.g. "petagent@<DEVICE_IP>"
    binding_overrides: HashMap<String, String>,  // action -> control
}

#[tauri::command]
async fn install_clawpkg_over_ssh(input: InstallSshInput)
    -> Result<InstallClawpkgResult, String>
{
    // 1. validate clawpkg locally (same validate_clawpkg_at_path)
    // 2. parse buttons.json, apply binding_overrides:
    //    for each (action, new_control) in overrides:
    //      find buttons entry by action, replace control + canonical event for that control
    // 3. tar widget into temp file
    // 4. ssh + scp: extract to /opt/board-runtime/widgets/<id>/
    // 5. ssh: echo "<id>" | sudo tee /opt/board-runtime/.active-widget
    // 6. return InstallClawpkgResult
}
```

### ComponentCenter UI

- Existing "推到设备" button currently calls `install_clawpkg_over_usb`.
- Detect device transport: if binding mode is "WiFi / MQTT" or "SSH", use `install_clawpkg_over_ssh`; else USB.
- Existing "按钮设置" UI's `bindingOverrides` now passed to SSH install instead of just changing footer string.
- New device-config field: `sshHost` (set during device setup, defaults to discovered mDNS or manual IP+user).

## E2E flow (after build)

```
1. user opens ComponentCenter, clicks "生成组件" textarea, types "会议计时"
2. updated skill generates 会议计时.clawpkg with:
   - negative-screen.json (initial 10 slot values)
   - buttons.json (action↔control mapping)
   - runtime/widget.json (states + transitions + tick)
3. user (optional) clicks "按钮设置", swaps 开始/暂停 from 红钮 to 屏幕
4. user clicks "推到设备"
5. Tauri install_clawpkg_over_ssh:
   - validate clawpkg
   - apply binding override to buttons.json
   - scp to petagent@<DEVICE_IP>:/opt/board-runtime/widgets/meeting-timer/
   - ssh write .active-widget = "meeting-timer"
6. Pi board-widget-runtime inotify trigger:
   - cancel previous widget loop
   - load meeting-timer widget.json + buttons.json
   - validate
   - state = "idle", elapsed_s = 0, target_s = 1500
   - render dashboard → write .stats-display
7. fb-display.sh picks up, fb-stats-renderer.py paints fb1: "📅 会议计时 / 未开始 / 已用时长 00:00 / 红钮 开始..."
8. user presses red button on physical device
9. board-rotary-input writes {control:"红色按钮", event:"button.primary.short_press"} to .current-event
10. board-widget-runtime sees event → buttons.json lookup → action = "timer.start_pause"
    → transitions: from="idle", on="timer.start_pause", to="running" → state = "running"
    → re-render: headline = "计时中", footer = "红钮 暂停 · 长按 重置" → write .stats-display
11. tick fires every 1s while running: elapsed_s += 1 → metricValue = "00:01" → "00:02" → ...
12. user long-presses red button → transition resets state=idle, elapsed_s=0, re-render
```

## Phasing

| Phase | Deliverable |
|---|---|
| P1 | Design spec (this doc), board-widget-runtime.py with state machine + tick (no fetchers/readers), updated skill (1 example: meeting-timer), JSON Schema validator on device, E2E test on Pi |
| P2 | install_clawpkg_over_ssh Tauri command + ComponentCenter UI wiring + button override OTA |
| P3 | Fetchers + readers + allowlist config, regenerate built-in examples (slack-off-countdown / tomato-clock / drink-reminder / token-usage) |
| P4 | v2 grammar (conditionals, cross-widget emit) if real widgets demand it |

This document covers P1+P2 fully. P3+P4 are extensions on the same architecture.
