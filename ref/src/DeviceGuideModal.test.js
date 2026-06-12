/**
 * [Input] DeviceGuideModal.jsx and lib/device-guide-content.js source.
 * [Output] Static Node coverage that the device guide focuses on the front red encoder knob/button and shows component-center screen button-function customization.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));

function readSource(fileName) {
  return readFileSync(join(srcDir, fileName), "utf8");
}

test("device guide documents the available hardware controls", () => {
  const content = readSource("lib/device-guide-content.js");
  const modal = readSource("DeviceGuideModal.jsx");

  assert.match(content, /encoder:\s*\{ emoji: "🔴", name: "屏幕前红色编码旋钮（可按压）" \}/);
  assert.match(content, /短按屏幕前红色编码旋钮 = 切到另一页/);
  assert.match(content, /转动屏幕前红色编码旋钮/);
  assert.match(content, /设备上的可用控件/);
  assert.match(content, /组件中心安装时可把屏幕点击\/长按绑定给当前 widget/);
  assert.match(content, /屏幕滑动仍用于切屏/);
  assert.match(content, /按钮功能面板/);
  assert.match(content, /buttons\.json/);
  assert.doesNotMatch(content, /设备顶部红色按钮/);
  assert.doesNotMatch(content, /顶部红钮/);
  assert.equal(content.includes('{ control: "topButton", gesture: "短按", action: "开始 / 暂停" }'), false);
  assert.doesNotMatch(content, /primary:\s*\{ emoji: "🔴", name: "红色按钮" \}/);
  assert.match(modal, /CONTROLS\[card\.canonicalControl\]/);
});
