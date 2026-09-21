/**
 * [Input] component-center/button-config.js pure option and P4 downlink helpers.
 * [Output] Regression coverage for model-v9 signaling, exact-default-only board
 *          migration, unique component controls, and repeatable optional global-exit resolution.
 * [Pos] test node in pc/src/component-center
 * [Sync] If this file changes, update `pc/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  COMPONENT_CONTROL_OPTIONS,
  DEVICE_BUTTON_CONFIG_MODEL_VERSION,
  componentInputEventSlots,
  defaultControlLabelForBinding,
  globalExitControlLabel,
  migrateP4V7ShippedBoardDefaults,
  resolveGlobalExitEvents,
} from "./button-config.js";

test("device button config model advances for joystick up/down navigation defaults", () => {
  assert.equal(DEVICE_BUTTON_CONFIG_MODEL_VERSION, 9);
});

test("the exact shipped v7 board map migrates SW1/SW3 without changing custom maps", () => {
  const bindings = [
    ["button.sw1.short_press", "page_back"],
    ["button.sw1.long_press", "disabled"],
    ["button.sw1.hold", "voice_ptt"],
    ["button.sw2.short_press", "component_center"],
    ["button.sw2.long_press", "disabled"],
    ["button.sw2.hold", "disabled"],
    ["button.sw3.short_press", "page_enter"],
    ["button.sw3.long_press", "disabled"],
    ["button.sw3.hold", "disabled"],
    ["button.encoder.short_press", "page_enter"],
    ["button.encoder.long_press", "disabled"],
    ["button.encoder.hold", "disabled"],
    ["knob.rotate_cw", "session_next"],
    ["knob.rotate_ccw", "session_previous"],
    ["joystick.up", "disabled"],
    ["joystick.down", "disabled"],
  ].map(([event, action]) => ({ event, action, value: "" }));
  const migration = migrateP4V7ShippedBoardDefaults(
    { config: { version: 7, bindings } },
    "esp-p4",
  );

  assert.equal(migration.migrated, true);
  assert.equal(migration.response.config.version, 9);
  assert.equal(
    migration.response.config.bindings.find((binding) => binding.event === "button.sw1.short_press")?.action,
    "page_enter",
  );
  assert.equal(
    migration.response.config.bindings.find((binding) => binding.event === "button.sw3.short_press")?.action,
    "page_back",
  );

  const customized = structuredClone({ config: { version: 7, bindings } });
  customized.config.bindings.find((binding) => binding.event === "joystick.up").action = "session_previous";
  const preserved = migrateP4V7ShippedBoardDefaults(customized, "esp-p4");
  assert.equal(preserved.migrated, false);
  assert.equal(preserved.response, customized);
});

test("component exit follows every authoritative board page-back binding", () => {
  assert.deepEqual(resolveGlobalExitEvents({ bindings: [] }), []);
  assert.deepEqual(
    resolveGlobalExitEvents({
      config: {
        bindings: [
          { event: "button.sw1.short_press", action: "page_back" },
          { event: "button.sw3.short_press", action: "page_back" },
          { event: "button.sw3.short_press", action: "page_back" },
          { event: "button.sw2.long_press", action: "page_back" },
          { event: "screen.region.tap", action: "page_back" },
        ],
      },
    }),
    ["button.sw1.short_press", "button.sw3.short_press", "button.sw2.long_press"],
  );
  assert.equal(globalExitControlLabel("button.sw2.long_press"), "SW2 长按");
});

test("component controls expose screen, switches, and joystick events without duplicates", () => {
  const events = COMPONENT_CONTROL_OPTIONS.map((option) => option.event);
  assert.equal(new Set(events).size, events.length);
  assert.ok(events.includes("screen.region.tap"));
  assert.ok(events.includes("button.sw1.short_press"));
  assert.ok(events.includes("button.encoder.long_press"));
  assert.equal(events.some((event) => /button\.sw[123]\.long_press/.test(event)), false);
  assert.ok(events.includes("knob.rotate_cw"));
  assert.ok(events.includes("knob.rotate_ccw"));
  assert.ok(events.includes("knob.rotate_cw / knob.rotate_ccw"));
  assert.ok(events.includes("joystick.up"));
  assert.ok(events.includes("joystick.down"));
});

test("package bindings resolve to the joystick-aware component-center labels", () => {
  assert.equal(
    defaultControlLabelForBinding({ control: "SW2", event: "button.sw2.short_press" }),
    "SW2 短按",
  );
  assert.equal(
    defaultControlLabelForBinding({ control: "屏幕区域", event: "screen.region.tap" }),
    "屏幕点击",
  );
  assert.equal(
    defaultControlLabelForBinding({
      control: "前方旋钮",
      event: "knob.rotate_cw / knob.rotate_ccw",
    }),
    "摇杆左右方向",
  );
  assert.equal(
    defaultControlLabelForBinding({ control: "前方摇杆", event: "joystick.up" }),
    "摇杆向上",
  );
  assert.deepEqual(
    componentInputEventSlots("knob.rotate_cw / knob.rotate_ccw"),
    ["knob.rotate_cw", "knob.rotate_ccw"],
  );
});
