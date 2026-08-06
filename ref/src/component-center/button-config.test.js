/**
 * [Input] component-center/button-config.js pure option and P4 downlink helpers.
 * [Output] Regression coverage for model-v7 signaling, exact-default-only board
 *          migration, unique component controls, and retained legacy full-map helpers.
 * [Pos] test node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  COMPONENT_CONTROL_OPTIONS,
  COMPONENT_SYSTEM_ACTION_PAGE_MAIN,
  DEFAULT_COMPONENT_GLOBAL_EXIT_EVENT,
  DEVICE_BUTTON_CONFIG_MODEL_VERSION,
  P4_COMPONENT_BUTTON_EVENTS,
  buildComponentButtonConfigBindings,
  buildComponentButtonConfigSnapshot,
  componentInputEventSlots,
  defaultControlLabelForBinding,
  migrateP4V6ShippedBoardDefaults,
  resolveGlobalExitEvent,
} from "./button-config.js";

test("device button config model advances for the SW1-back/SW3-confirm defaults", () => {
  assert.equal(DEVICE_BUTTON_CONFIG_MODEL_VERSION, 7);
});

test("the exact shipped v6 board map migrates SW1/SW3 without changing custom maps", () => {
  const bindings = [
    ["button.sw1.short_press", "page_enter"],
    ["button.sw1.long_press", "disabled"],
    ["button.sw1.hold", "voice_ptt"],
    ["button.sw2.short_press", "component_center"],
    ["button.sw2.long_press", "disabled"],
    ["button.sw2.hold", "disabled"],
    ["button.sw3.short_press", "page_back"],
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
  const migration = migrateP4V6ShippedBoardDefaults(
    { config: { version: 6, bindings } },
    "esp-p4",
  );

  assert.equal(migration.migrated, true);
  assert.equal(migration.response.config.version, 7);
  assert.equal(
    migration.response.config.bindings.find((binding) => binding.event === "button.sw1.short_press")?.action,
    "page_back",
  );
  assert.equal(
    migration.response.config.bindings.find((binding) => binding.event === "button.sw3.short_press")?.action,
    "page_enter",
  );

  const customized = structuredClone({ config: { version: 7, bindings } });
  customized.config.bindings.find((binding) => binding.event === "joystick.up").action = "session_previous";
  const preserved = migrateP4V6ShippedBoardDefaults(customized, "esp-p4");
  assert.equal(preserved.migrated, false);
  assert.equal(preserved.response, customized);
});

test("component exit follows the authoritative board page-back binding", () => {
  assert.equal(resolveGlobalExitEvent({ bindings: [] }), DEFAULT_COMPONENT_GLOBAL_EXIT_EVENT);
  assert.equal(
    resolveGlobalExitEvent({
      config: {
        bindings: [
          { event: "button.sw1.short_press", action: "page_enter" },
          { event: "button.sw3.short_press", action: "page_back" },
        ],
      },
    }),
    "button.sw3.short_press",
  );
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
  assert.equal(snapshot.buttonModelVersion, 7);
});
