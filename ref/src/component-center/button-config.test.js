/**
 * [Input] component-center/button-config.js pure option and P4 downlink helpers.
 * [Output] Regression coverage for unique component controls and retained legacy full-map helpers.
 * [Pos] test node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  COMPONENT_CONTROL_OPTIONS,
  COMPONENT_SYSTEM_ACTION_PAGE_MAIN,
  DEVICE_BUTTON_CONFIG_MODEL_VERSION,
  P4_COMPONENT_BUTTON_EVENTS,
  buildComponentButtonConfigBindings,
  buildComponentButtonConfigSnapshot,
  componentInputEventSlots,
  defaultControlLabelForBinding,
} from "./button-config.js";

test("device button config model advances for four-direction joystick support", () => {
  assert.equal(DEVICE_BUTTON_CONFIG_MODEL_VERSION, 5);
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

test("legacy P4 component map helper remains deterministic for compatibility callers", () => {
  const bindings = buildComponentButtonConfigBindings([
    { action: "game.score", event: "button.sw1.short_press" },
    { action: "game.choose_next", event: "knob.rotate_cw" },
    { action: COMPONENT_SYSTEM_ACTION_PAGE_MAIN, event: "button.encoder.long_press" },
  ]);
  assert.equal(P4_COMPONENT_BUTTON_EVENTS.length, 9);
  assert.equal(bindings.length, 16);
  assert.deepEqual(
    bindings.find((binding) => binding.event === "button.sw1.short_press"),
    { event: "button.sw1.short_press", action: "miniapp_action", value: "game.score" },
  );
  assert.deepEqual(
    bindings.find((binding) => binding.event === "button.sw2.short_press"),
    { event: "button.sw2.short_press", action: "disabled", value: "" },
  );
  assert.deepEqual(
    bindings.find((binding) => binding.event === "button.sw1.hold"),
    { event: "button.sw1.hold", action: "disabled", value: "" },
  );
  assert.deepEqual(
    bindings.find((binding) => binding.event === "button.encoder.long_press"),
    { event: "button.encoder.long_press", action: "page_main", value: "" },
  );
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
  const bothDirections = buildComponentButtonConfigBindings([
    { action: "game.adjust", event: "knob.rotate_cw / knob.rotate_ccw" },
  ]);
  assert.equal(
    bothDirections.find((binding) => binding.event === "knob.rotate_cw")?.value,
    "game.adjust",
  );
  assert.equal(
    bothDirections.find((binding) => binding.event === "knob.rotate_ccw")?.value,
    "game.adjust",
  );
});

test("ACKed component bindings replace the dashboard snapshot with the same miniapp actions", () => {
  const snapshot = buildComponentButtonConfigSnapshot(
    [
      {
        action: "game.score",
        event: "button.sw1.short_press",
        label: "SW1得分",
      },
      {
        action: "game.restart",
        event: "screen.region.long_press",
        label: "重新开始",
      },
      {
        action: COMPONENT_SYSTEM_ACTION_PAGE_MAIN,
        event: "button.encoder.long_press",
        label: "返回桌宠",
      },
    ],
    {
      buttonActions: {
        p4_sw1_short: "disabled",
        p4_sw2_short: "agent_enter",
      },
      buttonValues: {},
    },
  );

  assert.equal(snapshot.enabled, false);
  assert.equal(snapshot.buttonActions.p4_sw1_short, "miniapp_action");
  assert.equal(snapshot.buttonValues.p4_sw1_short, "game.score");
  assert.equal(snapshot.buttonLabels.p4_sw1_short, "SW1得分");
  assert.equal(snapshot.buttonActions.p4_sw2_short, "disabled");
  assert.equal(snapshot.buttonValues.p4_sw2_short, undefined);
  assert.equal(snapshot.buttonActions.p4_encoder_long, "page_main");
  assert.equal(snapshot.buttonValues.p4_encoder_long, undefined);
  assert.equal(snapshot.buttonActions.p4_encoder_cw, "disabled");
  assert.equal(snapshot.buttonActions.p4_joystick_up, "disabled");
  assert.equal(snapshot.buttonActions.p4_joystick_down, "disabled");
  assert.equal(snapshot.buttonModelVersion, 5);
});
