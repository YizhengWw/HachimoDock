/**
 * [Input] The binding-label model exported by `./binding-labels.js`.
 * [Output] Behavior coverage (bare node) for the two label vocabularies and the
 *          install-time CONTROL_OPTIONS resolution helpers ComponentCenter uses.
 * [Pos] test node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  CONTROL_HELP,
  CONTROL_OPTIONS,
  bindingKey,
  defaultControlLabelForBinding,
  formatBindingControl,
  isRoutedWidgetBinding,
  optionForControlLabel,
} from "./binding-labels.js";

test("formatBindingControl renders the rich status label and retires hardware buttons", () => {
  assert.equal(formatBindingControl({ event: "screen.region.tap" }), "负一屏屏幕 · 点击");
  assert.equal(formatBindingControl({ event: "knob.rotate_cw / knob.rotate_ccw" }), "前方旋钮 · 旋转");
  assert.equal(formatBindingControl({ event: "button.primary.short_press" }), "已停用硬件输入");
  assert.equal(formatBindingControl({}), "未指定");
});

test("isRoutedWidgetBinding is true only for screen-region events", () => {
  assert.equal(isRoutedWidgetBinding({ event: "screen.region.tap" }), true);
  assert.equal(isRoutedWidgetBinding({ event: "knob.rotate_cw" }), false);
});

test("CONTROL_OPTIONS only exposes the two bindable screen gestures", () => {
  assert.deepEqual(CONTROL_OPTIONS.map((o) => o.event), ["screen.region.tap", "screen.region.long_press"]);
  const labels = CONTROL_OPTIONS.map((o) => o.label).join(" ");
  assert.doesNotMatch(labels, /顶部红钮短按/);
  assert.doesNotMatch(labels, /旋钮旋转/);
});

test("CONTROL_HELP maps every option label to its help text", () => {
  for (const option of CONTROL_OPTIONS) assert.equal(CONTROL_HELP[option.label], option.help);
});

test("bindingKey composes a stable component:action key", () => {
  assert.equal(bindingKey("token-meter", "screen.region.tap"), "token-meter:screen.region.tap");
});

test("defaultControlLabelForBinding resolves by exact, then event, then control, then default", () => {
  assert.equal(defaultControlLabelForBinding({ control: "屏幕区域", event: "screen.region.tap" }), "屏幕点击");
  assert.equal(defaultControlLabelForBinding({ event: "screen.region.long_press" }), "屏幕长按");
  assert.equal(defaultControlLabelForBinding({ control: "怪控件" }), "怪控件");
  assert.equal(defaultControlLabelForBinding({}), CONTROL_OPTIONS[0].label);
});

test("optionForControlLabel finds the matching option or null", () => {
  assert.equal(optionForControlLabel("屏幕点击").event, "screen.region.tap");
  assert.equal(optionForControlLabel("不存在"), null);
});
