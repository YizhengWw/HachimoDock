/**
 * [Input] DeviceGuideModal.jsx, lib/device-guide-content.js, and styles.css source.
 * [Output] Static Node coverage for the compact three-step guide, accessible shared
 *          controls, all twelve P4 gestures, and responsive hardware-shaped layout.
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

test("device guide documents P4 navigation and all twelve configurable gestures", () => {
  const content = readSource("lib/device-guide-content.js");
  const modal = readSource("DeviceGuideModal.jsx");

  assert.match(content, /screenIds: \["main", "components"\]/);
  assert.match(content, /左右切换，中按进入；上下方向可独立配置/);
  assert.match(content, /\{ gesture: "向上", action: "可独立配置" \}/);
  assert.match(content, /\{ gesture: "向下", action: "可独立配置" \}/);
  assert.match(content, /\{ gesture: "向左", action: "上一个" \}/);
  assert.match(content, /\{ gesture: "向右", action: "下一个" \}/);
  assert.match(content, /\{ gesture: "中按短按", action: "确认 \/ 进入" \}/);
  assert.match(content, /\{ gesture: "中按长按", action: "暂不绑定" \}/);
  assert.match(content, /export const P4_CARDS/);
  assert.match(content, /共 12 个独立入口/);
  assert.match(content, /组件按键只在该组件打开时生效/);
  assert.match(content, /control: "sw1"[\s\S]{0,160}短按", action: "确认 \/ 进入/);
  assert.match(content, /control: "sw2"[\s\S]{0,160}短按", action: "组件中心/);
  assert.match(content, /control: "sw3"[\s\S]{0,160}短按", action: "返回（取消）/);
  assert.match(content, /control: "sw1"[\s\S]{0,200}gesture: "长按"[\s\S]{0,100}按住说话/);
  assert.match(modal, /runtime\)\.toLowerCase\(\) === "esp-p4" \? P4_CARDS : CARDS/);
});

test("device guide uses direct step navigation and shared accessible controls", () => {
  const modal = readSource("DeviceGuideModal.jsx");

  assert.match(modal, /role="dialog"/);
  assert.match(modal, /aria-modal="true"/);
  assert.match(modal, /className="device-guide-steps"/);
  assert.match(modal, /role="tablist"/);
  assert.match(modal, /role="tab"/);
  assert.match(modal, /aria-selected=\{candidateIndex === index\}/);
  assert.match(modal, /<Switch[\s\S]*label="下次不再自动弹出"/);
  assert.match(modal, /<Button[\s\S]*上一页/);
  assert.match(modal, /下一页/);
  assert.match(modal, /完成/);
  assert.doesNotMatch(modal, /device-guide-dots/);
  assert.doesNotMatch(modal, /device-guide-skip/);
  assert.doesNotMatch(modal, /device-guide-other-toggle/);
});

test("device guide layout is compact, responsive, and motion-aware", () => {
  const styles = readSource("styles.css");

  assert.match(styles, /\.device-guide-modal \{\s*width: min\(680px, 100%\)/);
  assert.match(styles, /\.device-guide-steps \{[\s\S]*grid-template-columns: repeat\(3/);
  assert.match(styles, /\.device-guide-gesture-grid \{[\s\S]*grid-template-columns: repeat\(4/);
  assert.match(styles, /\.device-guide-control-grid \{[\s\S]*grid-template-columns: repeat\(3/);
  assert.match(styles, /@media \(max-width: 640px\)[\s\S]*\.device-guide-gesture-grid \{[\s\S]*repeat\(2/);
  assert.match(styles, /@media \(prefers-reduced-motion: reduce\)/);
  assert.doesNotMatch(styles, /\.device-guide-screen-emoji/);
});
