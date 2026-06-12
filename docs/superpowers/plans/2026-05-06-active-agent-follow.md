# Active Agent Follow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let users select exactly one primary agent CLI for the desktop bridge and T113 device to follow, including onboarding selection and later dashboard switching.

**Architecture:** Add `activeAgentId` to the Tauri bridge profile and treat it as the source of truth. Keep `enabledAgents` only as compatibility output, always synchronized to `[activeAgentId]`. The board keeps following `state/active`; the desktop bridge makes that active state deterministic by starting only the selected agent listener.

**Tech Stack:** Tauri 2 Rust command layer, React 18 + Vite, Node `node:test` for frontend helper tests, Rust unit tests through Cargo.

---

## File Structure

- Modify `ref/src-tauri/src/lib.rs`: add `active_agent_id`, profile normalization/migration, env flag helper, and tests.
- Create `ref/src/agent-selection.js`: fixed agent metadata, merge logic, selectability, and default primary selection.
- Create `ref/src/agent-selection.test.js`: Node unit tests for fixed options and default selection.
- Modify `ref/src/DeviceSetup.jsx`: replace the completed step with the agent-selection onboarding page, persist `activeAgentId`, restart bridge, then call `onComplete`.
- Modify `ref/src/DeviceDashboard.jsx`: replace checkbox multi-select with radio-style primary-agent selection.
- Modify `ref/src/styles.css`: add reusable agent-selection grid/card styles for onboarding and dashboard.
- Modify `ref/src/.folder.md`: document the new shared agent-selection helper and single-primary-agent flow.
- Modify `ref/.folder.md`: document the bridge profile `activeAgentId` ownership.

---

### Task 1: Tauri Profile Source Of Truth

**Files:**
- Modify: `ref/src-tauri/src/lib.rs`
- Test: `ref/src-tauri/src/lib.rs`

- [ ] **Step 1: Write failing Rust tests**

Add these tests inside the existing `#[cfg(test)] mod tests` block in `ref/src-tauri/src/lib.rs`:

```rust
    #[test]
    fn normalize_bridge_profile_backfills_active_agent_from_enabled_agents() {
        let profile = normalize_bridge_profile(BridgeProfileFile {
            enabled_agents: vec!["codex".to_string(), "openclaw".to_string()],
            ..BridgeProfileFile::default()
        });

        assert_eq!(profile.active_agent_id, "codex");
        assert_eq!(profile.enabled_agents, vec!["codex".to_string()]);
    }

    #[test]
    fn normalize_bridge_profile_keeps_only_known_active_agent() {
        let profile = normalize_bridge_profile(BridgeProfileFile {
            active_agent_id: "not installed".to_string(),
            enabled_agents: vec!["codex".to_string()],
            ..BridgeProfileFile::default()
        });

        assert_eq!(profile.active_agent_id, "");
        assert!(profile.enabled_agents.is_empty());
    }

    #[test]
    fn normalize_bridge_profile_syncs_enabled_agents_to_active_agent() {
        let profile = normalize_bridge_profile(BridgeProfileFile {
            active_agent_id: "openclaw".to_string(),
            enabled_agents: vec!["codex".to_string()],
            ..BridgeProfileFile::default()
        });

        assert_eq!(profile.active_agent_id, "openclaw");
        assert_eq!(profile.enabled_agents, vec!["openclaw".to_string()]);
    }

    #[test]
    fn runtime_env_flags_enable_only_active_openclaw() {
        let profile = normalize_bridge_profile(BridgeProfileFile {
            active_agent_id: "openclaw".to_string(),
            ..BridgeProfileFile::default()
        });
        let flags = resolve_agent_runtime_env_flags(&profile);

        assert!(!flags.claude_log_monitor);
        assert!(!flags.sync_hooks);
        assert!(!flags.codex_monitor);
        assert!(flags.openclaw);
    }

    #[test]
    fn runtime_env_flags_enable_only_active_codex() {
        let profile = normalize_bridge_profile(BridgeProfileFile {
            active_agent_id: "codex".to_string(),
            ..BridgeProfileFile::default()
        });
        let flags = resolve_agent_runtime_env_flags(&profile);

        assert!(!flags.claude_log_monitor);
        assert!(!flags.sync_hooks);
        assert!(flags.codex_monitor);
        assert!(!flags.openclaw);
    }
```

- [ ] **Step 2: Run tests to verify RED**

Run:

```bash
cargo test
```

From:

```text
ref/src-tauri
```

Expected: FAIL to compile because `BridgeProfileFile` has no `active_agent_id`, and `resolve_agent_runtime_env_flags` is undefined.

- [ ] **Step 3: Add profile field and normalization helpers**

In `BridgeProfileFile` and `BridgeProfileResponse`, add:

```rust
    active_agent_id: String,
```

In `BridgeProfileInput`, add the optional input field so older callers can keep omitting it:

```rust
    active_agent_id: Option<String>,
```

Add these helpers near `normalize_pet_channel_id`:

```rust
#[derive(Debug, Clone, Copy)]
struct AgentRuntimeEnvFlags {
    claude_log_monitor: bool,
    sync_hooks: bool,
    codex_monitor: bool,
    openclaw: bool,
}

fn normalize_agent_id(value: &str) -> String {
    match value.trim().to_lowercase().as_str() {
        "claude-code" | "claude" => "claude-code".to_string(),
        "codex" => "codex".to_string(),
        "openclaw" => "openclaw".to_string(),
        _ => String::new(),
    }
}

fn resolve_active_agent_id(profile: &BridgeProfileFile) -> String {
    let explicit = normalize_agent_id(&profile.active_agent_id);
    if !explicit.is_empty() {
        return explicit;
    }

    profile
        .enabled_agents
        .iter()
        .map(|agent_id| normalize_agent_id(agent_id))
        .find(|agent_id| !agent_id.is_empty())
        .unwrap_or_default()
}

fn resolve_agent_runtime_env_flags(profile: &BridgeProfileFile) -> AgentRuntimeEnvFlags {
    match profile.active_agent_id.as_str() {
        "claude-code" => AgentRuntimeEnvFlags {
            claude_log_monitor: true,
            sync_hooks: true,
            codex_monitor: false,
            openclaw: false,
        },
        "codex" => AgentRuntimeEnvFlags {
            claude_log_monitor: false,
            sync_hooks: false,
            codex_monitor: true,
            openclaw: false,
        },
        "openclaw" => AgentRuntimeEnvFlags {
            claude_log_monitor: false,
            sync_hooks: false,
            codex_monitor: false,
            openclaw: true,
        },
        _ => AgentRuntimeEnvFlags {
            claude_log_monitor: false,
            sync_hooks: false,
            codex_monitor: false,
            openclaw: false,
        },
    }
}
```

- [ ] **Step 4: Normalize and respond with active agent**

Update `normalize_bridge_profile` after `pet_channel_id` normalization:

```rust
    profile.active_agent_id = resolve_active_agent_id(&profile);
    profile.enabled_agents = if profile.active_agent_id.is_empty() {
        Vec::new()
    } else {
        vec![profile.active_agent_id.clone()]
    };
```

Update `build_bridge_profile_response`:

```rust
        active_agent_id: profile.active_agent_id,
```

Update all `BridgeProfileFile` constructors in `save_device_binding`, `save_bridge_profile`, and profile recovery to include:

```rust
            active_agent_id: String::new(),
```

In `save_bridge_profile`, set the constructor's `active_agent_id` from input:

```rust
        active_agent_id: input.active_agent_id.unwrap_or_default(),
```

and keep `enabled_agents: input.enabled_agents.unwrap_or_default()`.

Update `bridge_profile_has_saved_values` so a profile containing only a future `activeAgentId` still counts as saved:

```rust
        || !profile.active_agent_id.trim().is_empty()
```

- [ ] **Step 5: Use active-agent env flags for bridge launch**

In `write_launch_script`, compute flags before `format!`:

```rust
    let flags = resolve_agent_runtime_env_flags(profile);
```

Add these exports to the generated script:

```sh
export CLAWD_ENABLE_CLAUDE_LOG_MONITOR={claude_log_monitor}
export CLAWD_SYNC_HOOKS={sync_hooks}
export CLAWD_ENABLE_CODEX_MONITOR={codex_monitor}
export OPENCLAW_ENABLE={openclaw_enable}
```

Use string substitutions:

```rust
        claude_log_monitor = shell_quote(if flags.claude_log_monitor { "true" } else { "false" }),
        sync_hooks = shell_quote(if flags.sync_hooks { "true" } else { "false" }),
        codex_monitor = shell_quote(if flags.codex_monitor { "true" } else { "false" }),
        openclaw_enable = shell_quote(if flags.openclaw { "true" } else { "false" }),
```

Remove the hard-coded script line:

```sh
export OPENCLAW_ENABLE='false'
```

In `start_bridge_direct`, replace the existing `enabled_agents` closure with:

```rust
    let flags = resolve_agent_runtime_env_flags(profile);
    command.env("CLAWD_ENABLE_CLAUDE_LOG_MONITOR", if flags.claude_log_monitor { "true" } else { "false" });
    command.env("CLAWD_SYNC_HOOKS", if flags.sync_hooks { "true" } else { "false" });
    command.env("CLAWD_ENABLE_CODEX_MONITOR", if flags.codex_monitor { "true" } else { "false" });
    command.env("OPENCLAW_ENABLE", if flags.openclaw { "true" } else { "false" });
```

- [ ] **Step 6: Run tests to verify GREEN**

Run:

```bash
cargo test
```

From:

```text
ref/src-tauri
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add ref/src-tauri/src/lib.rs
git commit -m "feat: persist active agent bridge profile"
```

---

### Task 2: Fixed Agent Selection Helper

**Files:**
- Create: `ref/src/agent-selection.js`
- Create: `ref/src/agent-selection.test.js`

- [ ] **Step 1: Write failing frontend helper tests**

Create `ref/src/agent-selection.test.js`:

```javascript
import assert from "node:assert/strict";
import test from "node:test";
import {
  FIXED_AGENT_IDS,
  mergeFixedAgentOptions,
  pickDefaultActiveAgentId,
} from "./agent-selection.js";

test("mergeFixedAgentOptions always returns fixed options in product order", () => {
  const options = mergeFixedAgentOptions([
    {
      id: "codex",
      label: "Codex",
      detected: true,
      ready: true,
      status: "ready",
      detail: "Codex ready",
    },
  ]);

  assert.deepEqual(options.map((option) => option.id), FIXED_AGENT_IDS);
  assert.equal(options.find((option) => option.id === "codex").selectable, true);
  assert.equal(options.find((option) => option.id === "claude-code").selectable, false);
  assert.equal(options.find((option) => option.id === "openclaw").label, "OpenClaw");
});

test("mergeFixedAgentOptions keeps unsupported fixed options disabled", () => {
  const options = mergeFixedAgentOptions([
    {
      id: "gemini-cli",
      label: "Gemini",
      detected: true,
      ready: true,
      status: "ready",
      detail: "Gemini ready",
    },
  ]);

  const gemini = options.find((option) => option.id === "gemini-cli");
  assert.equal(gemini.detected, true);
  assert.equal(gemini.runtimeSupported, false);
  assert.equal(gemini.selectable, false);
});

test("pickDefaultActiveAgentId prefers a saved selectable agent", () => {
  const options = mergeFixedAgentOptions([
    { id: "claude-code", detected: true, ready: true, status: "ready" },
    { id: "codex", detected: true, ready: true, status: "ready" },
  ]);

  assert.equal(pickDefaultActiveAgentId(options, "codex"), "codex");
});

test("pickDefaultActiveAgentId falls back to the first selectable option", () => {
  const options = mergeFixedAgentOptions([
    { id: "codex", detected: true, ready: true, status: "ready" },
    { id: "openclaw", detected: true, ready: true, status: "ready" },
  ]);

  assert.equal(pickDefaultActiveAgentId(options, "missing-agent"), "codex");
});
```

- [ ] **Step 2: Run tests to verify RED**

Run:

```bash
node --test src/agent-selection.test.js
```

From:

```text
ref
```

Expected: FAIL with module-not-found for `agent-selection.js`.

- [ ] **Step 3: Implement helper**

Create `ref/src/agent-selection.js`:

```javascript
const RUNTIME_SUPPORTED_AGENT_IDS = new Set(["claude-code", "codex", "openclaw"]);

export const FIXED_AGENT_OPTIONS = Object.freeze([
  { id: "claude-code", label: "Claude Code" },
  { id: "codex", label: "Codex" },
  { id: "openclaw", label: "OpenClaw" },
  { id: "copilot-cli", label: "Copilot" },
  { id: "gemini-cli", label: "Gemini" },
]);

export const FIXED_AGENT_IDS = Object.freeze(FIXED_AGENT_OPTIONS.map((option) => option.id));

export function normalizeAgentId(value) {
  const normalized = String(value || "").trim().toLowerCase();
  if (normalized === "claude") return "claude-code";
  return FIXED_AGENT_IDS.includes(normalized) ? normalized : "";
}

function normalizeText(value, fallback = "") {
  const normalized = String(value || "").trim();
  return normalized || String(fallback || "").trim();
}

function normalizeStatus(value, detected) {
  const normalized = normalizeText(value, detected ? "detected" : "not_found");
  return normalized || "not_found";
}

export function mergeFixedAgentOptions(detectedAgents = []) {
  const detectedMap = new Map(
    (Array.isArray(detectedAgents) ? detectedAgents : [])
      .map((agent) => [normalizeAgentId(agent?.id), agent])
      .filter(([id]) => Boolean(id)),
  );

  return FIXED_AGENT_OPTIONS.map((option) => {
    const detected = detectedMap.get(option.id) || {};
    const isDetected = Boolean(detected.detected);
    const ready = Boolean(detected.ready);
    const runtimeSupported = RUNTIME_SUPPORTED_AGENT_IDS.has(option.id);
    const selectable = Boolean(isDetected && ready && runtimeSupported);
    const status = normalizeStatus(detected.status, isDetected);
    const detail = normalizeText(
      detected.detail,
      isDetected
        ? runtimeSupported
          ? "已检测到"
          : "已检测到，但当前版本暂不支持作为主跟随 Agent"
        : "未检测到",
    );

    return {
      ...detected,
      id: option.id,
      label: option.label,
      detected: isDetected,
      ready,
      status,
      detail,
      runtimeSupported,
      selectable,
    };
  });
}

export function pickDefaultActiveAgentId(options = [], savedActiveAgentId = "") {
  const normalizedSaved = normalizeAgentId(savedActiveAgentId);
  const selectable = Array.isArray(options) ? options.filter((option) => option?.selectable) : [];

  if (normalizedSaved && selectable.some((option) => option.id === normalizedSaved)) {
    return normalizedSaved;
  }

  return selectable[0]?.id || "";
}

export function getSelectableAgentCount(options = []) {
  return (Array.isArray(options) ? options : []).filter((option) => option?.selectable).length;
}
```

- [ ] **Step 4: Run tests to verify GREEN**

Run:

```bash
node --test src/agent-selection.test.js
```

From:

```text
ref
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add ref/src/agent-selection.js ref/src/agent-selection.test.js
git commit -m "feat: add fixed agent selection helper"
```

---

### Task 3: Onboarding Primary Agent Step

**Files:**
- Modify: `ref/src/DeviceSetup.jsx`
- Modify: `ref/src/styles.css`

- [ ] **Step 1: Run existing helper tests before UI edits**

Run:

```bash
node --test src/agent-selection.test.js
```

From:

```text
ref
```

Expected: PASS. This confirms the selection rules before wiring the UI.

- [ ] **Step 2: Import selection helpers and icon**

In `ref/src/DeviceSetup.jsx`, add `Code` to lucide imports and import helpers:

```javascript
import {
  getSelectableAgentCount,
  mergeFixedAgentOptions,
  pickDefaultActiveAgentId,
} from "./agent-selection";
```

- [ ] **Step 3: Extend setup state and reducer**

Add to `INITIAL_STATE`:

```javascript
  agentScanLoading: false,
  agentScanError: "",
  agentOptions: [],
  selectedAgentId: "",
  savingAgent: false,
```

Add reducer cases:

```javascript
    case "set_agent_scan_loading":
      return { ...state, agentScanLoading: action.value, agentScanError: action.value ? "" : state.agentScanError };
    case "set_agent_options":
      return {
        ...state,
        agentOptions: action.options,
        selectedAgentId: pickDefaultActiveAgentId(action.options, action.selectedAgentId || state.selectedAgentId),
        agentScanLoading: false,
        agentScanError: "",
      };
    case "set_agent_scan_error":
      return { ...state, agentScanError: action.error, agentScanLoading: false };
    case "set_selected_agent":
      return { ...state, selectedAgentId: action.id };
    case "set_saving_agent":
      return { ...state, savingAgent: action.value };
```

- [ ] **Step 4: Add scan and save callbacks**

Inside `DeviceSetup`, add:

```javascript
  const scanAgents = useCallback(async (preferredAgentId = "") => {
    dispatch({ type: "set_agent_scan_loading", value: true });
    try {
      const profile = hasTauriRuntime() ? await invoke("load_bridge_profile") : { activeAgentId: "" };
      const response = hasTauriRuntime() ? await invoke("detect_local_agents") : { agents: [] };
      const options = mergeFixedAgentOptions(response.agents || []);
      dispatch({
        type: "set_agent_options",
        options,
        selectedAgentId: preferredAgentId || profile.activeAgentId || "",
      });
    } catch (err) {
      dispatch({ type: "set_agent_options", options: mergeFixedAgentOptions([]), selectedAgentId: "" });
      dispatch({ type: "set_agent_scan_error", error: String(err) });
    }
  }, []);

  const savePrimaryAgentAndComplete = useCallback(async () => {
    const selected = state.agentOptions.find((option) => option.id === state.selectedAgentId);
    if (!selected?.selectable) return;

    dispatch({ type: "set_saving_agent", value: true });
    try {
      if (hasTauriRuntime()) {
        const profile = await invoke("load_bridge_profile");
        await invoke("save_bridge_profile", {
          input: {
            desktopDeviceId: profile.desktopDeviceId,
            mqttUrl: profile.mqttUrl,
            mqttNamespace: profile.mqttNamespace,
            mqttUsername: profile.mqttUsername,
            mqttPassword: profile.mqttPassword,
            petChannelId: profile.petChannelId,
            activeAgentId: selected.id,
            enabledAgents: [selected.id],
          },
        });
        await invoke("ensure_bridge_runtime", { input: { forceRestart: true } });
      }
      dispatch({ type: "set_saving_agent", value: false });
      onComplete && onComplete();
    } catch (err) {
      dispatch({ type: "set_saving_agent", value: false });
      dispatch({ type: "set_agent_scan_error", error: `保存主 Agent 失败: ${err}` });
    }
  }, [onComplete, state.agentOptions, state.selectedAgentId]);
```

- [ ] **Step 5: Enter agent selection after MQTT verification succeeds**

In `submitConfig`, replace the `dispatch({ type: "set_result", ... })` success branch with:

```javascript
              dispatch({
                type: "set_result",
                ip: "",
                attempt: { ok: true, ssid },
              });
              await scanAgents();
              return;
```

Make sure `scanAgents` is included in the `submitConfig` dependency array.

- [ ] **Step 6: Remove automatic completion timer**

Remove this effect:

```javascript
  useEffect(() => {
    if (state.phase === "completed" && onComplete) {
      const timer = setTimeout(onComplete, 2000);
      return () => clearTimeout(timer);
    }
  }, [state.phase, onComplete]);
```

The user must explicitly choose an agent and click "完成绑定".

- [ ] **Step 7: Replace completed card content with agent selection**

Replace the `phase === "completed"` `WizardCard` with:

```jsx
      {phase === "completed" && (
        <WizardCard
          className="wizard-card--agent"
          eyebrow="第 3 步 / 共 3 步"
          title="选择桌宠跟随的Agent渠道"
          description="扫描本地 Agent 后，选择一个已检测到的渠道作为桌宠默认跟随对象。"
          footer={(
            <>
              <button className="btn-ghost" type="button" onClick={() => dispatch({ type: "set_phase", phase: "wait_user_input" })}>
                上一步
              </button>
              <button
                className="btn-primary"
                type="button"
                onClick={savePrimaryAgentAndComplete}
                disabled={!state.selectedAgentId || state.savingAgent}
              >
                {state.savingAgent ? <Loader size={14} className="spin" /> : "完成绑定"}
                <CheckCircle size={14} />
              </button>
            </>
          )}
        >
          <div className="agent-select-summary">
            {state.agentScanLoading ? (
              <>
                <Loader size={16} className="spin" />
                <span>正在扫描本地 Agent...</span>
              </>
            ) : (
              <span>
                已扫描到 {state.agentOptions.filter((agent) => agent.detected).length} 个本地渠道，可直接绑定 {getSelectableAgentCount(state.agentOptions)} 个。
              </span>
            )}
          </div>

          {state.agentScanError && (
            <div className="hotspot-warning">
              <XCircle size={16} />
              <span>{state.agentScanError}</span>
              <button className="btn-secondary btn-sm" type="button" onClick={() => scanAgents(state.selectedAgentId)}>
                重新扫描
              </button>
            </div>
          )}

          <div className="agent-select-grid">
            {state.agentOptions.map((agent) => {
              const selected = state.selectedAgentId === agent.id;
              return (
                <label
                  key={agent.id}
                  className={[
                    "agent-select-card",
                    selected ? "agent-select-card--selected" : "",
                    agent.selectable ? "" : "agent-select-card--disabled",
                  ].filter(Boolean).join(" ")}
                >
                  <input
                    type="radio"
                    name="primaryAgent"
                    checked={selected}
                    disabled={!agent.selectable}
                    onChange={() => dispatch({ type: "set_selected_agent", id: agent.id })}
                  />
                  <span className="agent-select-icon">
                    <Code size={18} />
                  </span>
                  <span className="agent-select-copy">
                    <strong>{agent.label}</strong>
                    <span>{agent.selectable ? "已检测到" : agent.detected ? agent.detail : "未检测到"}</span>
                  </span>
                  {selected ? <CheckCircle size={16} className="agent-select-check" /> : null}
                </label>
              );
            })}
          </div>
        </WizardCard>
      )}
```

- [ ] **Step 8: Add CSS for onboarding grid**

In `ref/src/styles.css`, near existing `.agent-list` styles, add:

```css
.wizard-card--agent {
  max-width: 720px;
}

.agent-select-summary {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 22px;
  color: var(--text-soft);
  font-weight: 600;
}

.agent-select-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
}

.agent-select-card {
  min-height: 86px;
  display: flex;
  align-items: center;
  gap: 14px;
  padding: 16px;
  border: 1px solid var(--line);
  border-radius: var(--radius-lg);
  background: var(--surface);
  transition: border-color 0.14s ease, background-color 0.14s ease, box-shadow 0.14s ease;
}

.agent-select-card input {
  position: absolute;
  opacity: 0;
  pointer-events: none;
}

.agent-select-card--selected {
  border-color: rgba(255, 103, 0, 0.28);
  background: #fffaf7;
  box-shadow: var(--shadow-xs);
}

.agent-select-card--disabled {
  color: var(--text-faint);
  background: var(--surface-muted);
}

.agent-select-icon {
  width: 32px;
  height: 32px;
  display: grid;
  place-items: center;
  border-radius: var(--radius-md);
  color: var(--text-on-accent);
  background: var(--accent);
  flex-shrink: 0;
}

.agent-select-card--disabled .agent-select-icon {
  opacity: 0.55;
}

.agent-select-copy {
  min-width: 0;
  display: flex;
  flex: 1;
  flex-direction: column;
  gap: 4px;
}

.agent-select-copy strong {
  color: var(--text);
  font-size: 15px;
}

.agent-select-copy span {
  color: var(--text-muted);
  font-size: 12px;
  line-height: 1.4;
}

.agent-select-check {
  color: var(--accent);
  flex-shrink: 0;
}
```

- [ ] **Step 9: Build to verify UI compile**

Run:

```bash
npm run build:web
```

From:

```text
ref
```

Expected: PASS.

- [ ] **Step 10: Commit**

```bash
git add ref/src/DeviceSetup.jsx ref/src/styles.css
git commit -m "feat: add onboarding primary agent selection"
```

---

### Task 4: Dashboard Single-Select Agent Control

**Files:**
- Modify: `ref/src/DeviceDashboard.jsx`
- Modify: `ref/src/styles.css`

- [ ] **Step 1: Run existing tests before dashboard edit**

Run:

```bash
node --test src/agent-selection.test.js
```

From:

```text
ref
```

Expected: PASS.

- [ ] **Step 2: Import shared helper**

In `ref/src/DeviceDashboard.jsx`, import:

```javascript
import {
  mergeFixedAgentOptions,
  pickDefaultActiveAgentId,
} from "./agent-selection";
```

- [ ] **Step 3: Rename state from enabled set to active agent**

Replace `enabledAgents` in `INITIAL_STATE` with:

```javascript
  activeAgentId: "",
```

Replace reducer cases:

```javascript
    case "set_agents":
      return {
        ...state,
        agents: action.agents,
        activeAgentId: pickDefaultActiveAgentId(action.agents, action.activeAgentId || state.activeAgentId),
        scanning: false,
      };
    case "set_active_agent":
      return { ...state, activeAgentId: action.id };
```

Remove `loadEnabledAgents` and `saveEnabledAgents` localStorage helpers. The bridge profile is the source of truth.

- [ ] **Step 4: Load activeAgentId from bridge profile**

Change the profile-loading effect to set:

```javascript
        dispatch({ type: "set_bridge_profile", value: profile });
        if (profile.activeAgentId) {
          dispatch({ type: "set_active_agent", id: profile.activeAgentId });
        }
```

- [ ] **Step 5: Scan fixed options**

Change the scan effect success branch to:

```javascript
        const agents = mergeFixedAgentOptions(response.agents || []);
        dispatch({ type: "set_agents", agents, activeAgentId: state.bridgeProfile?.activeAgentId || "" });
```

Use a local variable if React dependency warnings make `state.bridgeProfile` awkward:

```javascript
let savedActiveAgentId = "";
invoke("load_bridge_profile")
  .then((profile) => {
    savedActiveAgentId = profile.activeAgentId || "";
    dispatch({ type: "set_bridge_profile", value: profile });
    return invoke("detect_local_agents");
  })
  .then((response) => {
    const agents = mergeFixedAgentOptions(response.agents || []);
    dispatch({ type: "set_agents", agents, activeAgentId: savedActiveAgentId });
  })
```

- [ ] **Step 6: Save active agent to bridge**

Replace `syncEnabledAgentsToBridge` with:

```javascript
  const syncActiveAgentToBridge = useCallback((agentId) => {
    if (bridgeRestartTimer.current) clearTimeout(bridgeRestartTimer.current);
    bridgeRestartTimer.current = setTimeout(() => {
      invoke("load_bridge_profile")
        .then((profile) => {
          dispatch({ type: "set_bridge_profile", value: profile });
          return invoke("save_bridge_profile", {
            input: {
              desktopDeviceId: profile.desktopDeviceId,
              mqttUrl: profile.mqttUrl,
              mqttNamespace: profile.mqttNamespace,
              mqttUsername: profile.mqttUsername,
              mqttPassword: profile.mqttPassword,
              petChannelId: profile.petChannelId,
              activeAgentId: agentId,
              enabledAgents: agentId ? [agentId] : [],
            },
          });
        })
        .then(() => invoke("ensure_bridge_runtime", { input: { forceRestart: true } }))
        .then((r) => dispatch({ type: "set_bridge_running", value: !!r?.running }))
        .catch((err) => dispatch({ type: "set_test_result", ok: false, message: `切换主 Agent 失败: ${err}` }));
    }, 500);
  }, []);
```

Add:

```javascript
  const selectAgent = useCallback((agent) => {
    if (!agent?.selectable) return;
    dispatch({ type: "set_active_agent", id: agent.id });
    syncActiveAgentToBridge(agent.id);
  }, [syncActiveAgentToBridge]);
```

- [ ] **Step 7: Render radio-style fixed options**

Replace the checkbox `agent-list` rendering with:

```jsx
          <div className="agent-list">
            {state.agents.map((agent) => {
              const Icon = AGENT_ICONS[agent.id] || Code;
              const selected = state.activeAgentId === agent.id;
              const itemClassName = [
                "agent-item",
                selected ? "agent-item--enabled" : "",
                agent.selectable ? "" : "agent-item--disabled",
              ].filter(Boolean).join(" ");

              return (
                <label key={agent.id} className={itemClassName}>
                  <input
                    type="radio"
                    name="dashboardPrimaryAgent"
                    checked={selected}
                    disabled={!agent.selectable}
                    onChange={() => selectAgent(agent)}
                    style={{ display: "none" }}
                  />
                  <div className="agent-item__icon">
                    <Icon size={18} />
                  </div>
                  <div className="agent-item__content">
                    <div className="agent-item__title">{agent.label}</div>
                    <div className="agent-item__detail">
                      {agent.selectable ? agent.detail : agent.detected ? agent.detail : "未检测到"}
                    </div>
                  </div>
                  <div className={`agent-item__availability${agent.selectable ? " agent-item__availability--ok" : ""}`}>
                    {selected ? <CheckCircle size={18} /> : agent.selectable ? <CheckCircle size={18} /> : <XCircle size={18} />}
                  </div>
                </label>
              );
            })}
          </div>
```

- [ ] **Step 8: Update text copy**

Change section title copy:

```jsx
<h4>主跟随 Agent</h4>
<p>选择一个桌宠和设备端要跟随的 Agent。切换后 Bridge 会只监听这个 Agent。</p>
```

- [ ] **Step 9: Build and test**

Run:

```bash
node --test src/agent-selection.test.js
npm run build:web
```

From:

```text
ref
```

Expected: both PASS.

- [ ] **Step 10: Commit**

```bash
git add ref/src/DeviceDashboard.jsx ref/src/styles.css
git commit -m "feat: switch dashboard to primary agent selection"
```

---

### Task 5: Documentation And Full Verification

**Files:**
- Modify: `ref/src/.folder.md`
- Modify: `ref/.folder.md`

- [ ] **Step 1: Update folder docs**

In `ref/src/.folder.md`, update architecture text to mention:

```markdown
- Function: React source for the Pet Manager desktop bridge configurator, including device setup, onboarding primary-agent selection, and the bound-device dashboard shell.
```

Add file rows:

```markdown
| `agent-selection.js` | `runtime` | Fixed primary-agent option metadata and selection normalization shared by onboarding and dashboard |
| `agent-selection.test.js` | `test` | Node unit tests for fixed agent option merging and default primary-agent selection |
```

In `ref/.folder.md`, update the `Function` sentence to include:

```markdown
single-primary-agent bridge profile selection
```

- [ ] **Step 2: Run full desktop verification**

Run:

```bash
node --test src/agent-selection.test.js
npm run build:web
cargo test
```

Run the first two from `ref`, and `cargo test` from `ref/src-tauri`.

Expected:

- `node --test`: PASS
- `npm run build:web`: PASS
- `cargo test`: PASS

- [ ] **Step 3: Run device runtime regression tests**

Run:

```bash
cmake -S . -B build
cmake --build build
./build/runtime_tests
```

From:

```text
../board-runtime
```

Expected: `board-runtime tests passed`.

- [ ] **Step 4: Inspect final diff**

Run:

```bash
git status --short
git diff --stat HEAD
```

Expected: only planned files changed.

- [ ] **Step 5: Commit docs and any final fixes**

```bash
git add ref/.folder.md ref/src/.folder.md
git commit -m "docs: document active agent selection flow"
```

If final fixes are needed after verification, commit them with a focused message such as:

```bash
git add <fixed-files>
git commit -m "fix: stabilize active agent selection"
```
