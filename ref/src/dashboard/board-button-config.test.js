/**
 * [Input] The pure board-button / voice config model exported by
 *         `./board-button-config.js`.
 * [Output] Behavior coverage (bare node) for the contracts the device dashboard
 *          relies on: encoder rotation fixed to volume, no negative-screen touch
 *          rows, the 不绑定/disabled option, defensive voice-config normalization,
 *          and the OTA binding builder clamping unknown actions to each row default.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  BOARD_BUTTON_CONTROL_ROWS,
  BUTTON_FUNCTION_OPTIONS,
  DEFAULT_BUTTON_ACTIONS,
  DEFAULT_VOICE_CONFIG,
  VOICE_CONFIG_STORAGE_KEY,
  actionOptionById,
  buildBoardButtonConfigBindings,
  normalizeVoiceConfig,
} from "./board-button-config.js";

test("front encoder rotation is fixed to volume adjustment only", () => {
  const rotate = BOARD_BUTTON_CONTROL_ROWS.find((r) => r.id === "encoder_rotate");
  assert.deepEqual(rotate.actionOptions, ["volume_adjust"]);
  assert.equal(DEFAULT_BUTTON_ACTIONS.encoder_rotate, "volume_adjust");
  const bindings = buildBoardButtonConfigBindings({ encoder_rotate: "system_page" });
  assert.equal(bindings.find((b) => b.event === rotate.event).action, "volume_adjust");
});

test("negative-screen touch gestures are not exposed as button-config rows", () => {
  const ids = BOARD_BUTTON_CONTROL_ROWS.map((r) => r.id);
  assert.ok(!ids.includes("screen_tap") && !ids.includes("screen_long_press"));
  assert.doesNotMatch(BOARD_BUTTON_CONTROL_ROWS.map((r) => r.event).join(" "), /screen\.region/);
});

test('a "不绑定" (disabled) function option exists', () => {
  const disabled = BUTTON_FUNCTION_OPTIONS.find((o) => o.id === "disabled");
  assert.equal(disabled.label, "不绑定");
  assert.match(disabled.detail, /忽略该输入/);
});

test("actionOptionById falls back to the first option for unknown ids", () => {
  assert.equal(actionOptionById("volume_adjust").id, "volume_adjust");
  assert.equal(actionOptionById("nope").id, BUTTON_FUNCTION_OPTIONS[0].id);
});

test("buildBoardButtonConfigBindings clamps unknown actions to the row default", () => {
  const bindings = buildBoardButtonConfigBindings({ encoder_button_short: "totally-invalid" });
  assert.equal(bindings.length, BOARD_BUTTON_CONTROL_ROWS.length);
  const shortRow = BOARD_BUTTON_CONTROL_ROWS.find((r) => r.id === "encoder_button_short");
  assert.equal(bindings.find((b) => b.event === shortRow.event).action, shortRow.defaultAction);
  const ok = buildBoardButtonConfigBindings({ encoder_button: "voice_ptt" });
  const longRow = BOARD_BUTTON_CONTROL_ROWS.find((r) => r.id === "encoder_button");
  assert.equal(ok.find((b) => b.event === longRow.event).action, "voice_ptt");
});

test("normalizeVoiceConfig defends against missing / malformed values", () => {
  const fresh = normalizeVoiceConfig({});
  assert.equal(fresh.enabled, false);
  assert.equal(fresh.trigger, DEFAULT_VOICE_CONFIG.trigger);
  for (const row of BOARD_BUTTON_CONTROL_ROWS) {
    assert.ok(row.actionOptions.includes(fresh.buttonActions[row.id]));
  }
  const bad = normalizeVoiceConfig({ enabled: "yes", trigger: "top_button.hold" });
  assert.equal(bad.enabled, false);
  assert.equal(bad.trigger, DEFAULT_VOICE_CONFIG.trigger);
  const clamped = normalizeVoiceConfig({ buttonActions: { encoder_rotate: "system_page" } });
  assert.equal(clamped.buttonActions.encoder_rotate, "volume_adjust");
});

test("the voice-config storage key is a single source of truth", () => {
  assert.equal(VOICE_CONFIG_STORAGE_KEY, "pet-manager.board-voice-config");
});
