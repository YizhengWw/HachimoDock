/**
 * [Input] Component button bindings covering joystick directions and physical buttons.
 * [Output] Node coverage for mapping-aware, compact game-play guidance and legacy filtering.
 * [Pos] test node in pc/src/component-center
 * [Sync] If this file changes, update `pc/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { buildComponentPlayGuide } from "./binding-labels.js";

test("buildComponentPlayGuide groups joystick directions and keeps mapped actions readable", () => {
  const guide = buildComponentPlayGuide([
    { action: "jump", event: "joystick.up", label: "向上跳跃" },
    { action: "drop", event: "joystick.down", label: "快速下坠" },
    { action: "left", event: "knob.rotate_ccw", label: "向左移动" },
    { action: "right", event: "knob.rotate_cw", label: "向右移动" },
    { action: "dash", event: "button.sw2.short_press", label: "向前冲刺" },
    { action: "start", event: "button.sw1.short_press", label: "开始或重开" },
  ]);

  assert.equal(
    guide,
    "摇杆：向上跳跃、快速下坠、向左移动、向右移动；SW2：向前冲刺；SW1：开始或重开；退出跟随设备全局设置",
  );
});

test("buildComponentPlayGuide omits package-authored navigation and empty bindings", () => {
  assert.equal(buildComponentPlayGuide([]), "");
  assert.equal(buildComponentPlayGuide([
    { action: "page_back", event: "button.sw3.short_press", label: "返回" },
  ]), "");
});
