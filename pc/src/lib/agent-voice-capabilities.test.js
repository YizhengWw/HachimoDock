import test from "node:test";
import assert from "node:assert/strict";
import { deviceVoicePlatformWarning } from "./agent-voice-capabilities.js";

test("MiMoCode platform limitations are surfaced before starting voice recognition", () => {
  assert.match(deviceVoicePlatformWarning("mimocode", "windows"), /目前仅支持 macOS/);
  assert.match(deviceVoicePlatformWarning("mimocode", "other"), /目前仅支持 macOS/);
  assert.equal(deviceVoicePlatformWarning("mimocode", "macos"), "");
  for (const id of ["codex", "claude-code", "openclaw", "workbuddy"]) {
    assert.equal(deviceVoicePlatformWarning(id, "windows"), "");
    assert.equal(deviceVoicePlatformWarning(id, "macos"), "");
  }
});
