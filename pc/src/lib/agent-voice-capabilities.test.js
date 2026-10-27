import test from "node:test";
import assert from "node:assert/strict";
import { deviceVoicePlatformWarning, deviceVoiceTransportReady } from "./agent-voice-capabilities.js";

test("native Mac Codex draft path does not require the CLI transport", () => {
  assert.equal(deviceVoiceTransportReady("codex", "macos", false), true);
  assert.equal(deviceVoiceTransportReady("codex", "windows", false), false);
  assert.equal(deviceVoiceTransportReady("openclaw", "macos", false), false);
  assert.equal(deviceVoiceTransportReady("openclaw", "macos", true), true);
});

test("MiMoCode platform limitations are surfaced before starting voice recognition", () => {
  assert.match(deviceVoicePlatformWarning("mimocode", "windows"), /目前仅支持 macOS/);
  assert.match(deviceVoicePlatformWarning("mimocode", "other"), /目前仅支持 macOS/);
  assert.equal(deviceVoicePlatformWarning("mimocode", "macos"), "");
  for (const id of ["codex", "claude-code", "openclaw", "workbuddy"]) {
    assert.equal(deviceVoicePlatformWarning(id, "windows"), "");
    assert.equal(deviceVoicePlatformWarning(id, "macos"), "");
  }
});
