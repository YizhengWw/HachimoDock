/**
 * [Input] DeepSeek Harness production wiring in Rust discovery/allowlists and the managed bridge sidecar.
 * [Output] Static regression coverage that DSH registers with the agent-session-bus and stays out of visible-composer paths.
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

test("Rust keeps DeepSeek Harness on the bus-injection path", () => {
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
  // DSH is injected through agent-session-bus, so it must never join the AX
  // visible-composer set (which would write into a desktop editor instead).
  assert.doesNotMatch(
    rust,
    /fn agent_uses_visible_composer\(agent_id: &str\) -> bool \{\s*matches!\([^)]*deepseek-harness/,
  );
});
