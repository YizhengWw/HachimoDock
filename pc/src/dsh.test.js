/**
 * [Input] DeepSeek Harness production wiring in Rust discovery/allowlists and the managed bridge sidecar.
 * [Output] Static wiring checks: DSH status uses the managed bus, while voice uses the current visible composer with no bound-history fallback.
 * [Pos] test node in pc/src
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const read = (relativePath) => readFileSync(new URL(relativePath, import.meta.url), "utf8");

test("managed bridge sidecar registers the DeepSeek Harness adapter", () => {
  const bridge = read("../src-tauri/bridge/packages/clawd-backend-service/src/headless-mqtt.js");
  const busIndex = read("../src-tauri/bridge/packages/agent-session-bus/src/index.js");
  const busPackage = JSON.parse(read("../src-tauri/bridge/packages/agent-session-bus/package.json"));

  assert.match(busIndex, /DshAdapter/);
  assert.match(bridge, /^\s+DshAdapter,\s*$/m);
  assert.match(bridge, /new DshAdapter\(\{ log: busLog \}\)/);
  assert.equal(busPackage.exports["./adapters/dsh"], "./src/adapters/dsh.js");
});

test("Rust routes DeepSeek voice through the current visible composer, not bound history", () => {
  const rust = read("../src-tauri/src/lib.rs");

  assert.match(rust, /detect_dsh\(\),/);
  assert.match(rust, /KNOWN_USB_STATE_SOURCES: \[&str; 6\]/);
  assert.match(
    rust,
    /"deepseek-harness" \| "deepseek" \| "dsh" => Some\("deepseek-harness"\.to_string\(\)\)/,
  );
  assert.match(rust, /"deepseek-harness" => "deepseek-harness"\.to_string\(\)/);
  assert.match(
    rust,
    /"deepseek-harness" \| "deepseek" \| "dsh" => "DeepSeek Harness"\.to_string\(\)/,
  );
  // These are static native-wiring assertions, not an AX/UIA runtime test.
  const body = (name) => {
    const start = rust.indexOf(`fn ${name}(`);
    assert.notEqual(start, -1, `missing native contract ${name}`);
    const end = rust.indexOf("\n}", start);
    assert.notEqual(end, -1, `missing native contract boundary ${name}`);
    return rust.slice(start, end + 2);
  };
  assert.match(body("agent_uses_visible_composer"), /"deepseek-harness"/);
  assert.match(body("should_use_current_visible_session"), /matches!\(agent_id,[^\n]*"deepseek-harness"/);
  assert.match(body("device_voice_bound_target_is_addressable"),
    /!matches!\(target\.agent_id\.as_str\(\), "workbuddy" \| "deepseek-harness"\)/);
  assert.match(body("agent_streams_voice_draft"),
    /!matches!\(agent_id, "workbuddy" \| "deepseek-harness"\)/);
});

test("DSH interrupt action is wired end to end and gated by selected agent", () => {
  const rust = read("../src-tauri/src/lib.rs");
  const dashboard = read("DeviceDashboard.jsx");
  const panel = read("dashboard/BoardButtonPanel.jsx");
  assert.match(rust, /dsh_interrupt/);
  assert.match(rust, /fn interrupt_dsh_session/);
  assert.match(dashboard, /dsh_interrupt/);
  assert.match(panel, /selectedAgentId !== "deepseek-harness"/);
});
