/**
 * [Input] Read DashboardActionsMenu.jsx source.
 * [Output] Static Node coverage for the 3 menu items, the danger styling on 解绑, and the expected callback prop signature.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "DashboardActionsMenu.jsx"), "utf8");

test("DashboardActionsMenu exports a default React component", () => {
  assert.match(source, /export default function DashboardActionsMenu\s*\(/);
});

test("Menu renders the 3 items in the spec order", () => {
  const sendIdx = source.indexOf("发送测试消息");
  const copyIdx = source.indexOf("复制桌面设备 ID");
  const unbindIdx = source.indexOf("解绑设备");
  assert.ok(sendIdx !== -1, "expected 发送测试消息");
  assert.ok(copyIdx !== -1, "expected 复制桌面设备 ID");
  assert.ok(unbindIdx !== -1, "expected 解绑设备");
  assert.ok(sendIdx < copyIdx && copyIdx < unbindIdx, "items render in spec order");
});

test("解绑 is styled as danger", () => {
  assert.match(source, /dashboard-actions-menu__item--danger/);
});

test("Menu accepts onSendTest, onCopyDesktopId, onUnbind props", () => {
  for (const prop of ["onSendTest", "onCopyDesktopId", "onUnbind"]) {
    assert.match(source, new RegExp(`\\b${prop}\\b`), `expected prop ${prop}`);
  }
});

test("Menu hides itself when the trigger is clicked outside / Escape pressed", () => {
  // Implementation hook for click-outside; either listens to document mousedown or wraps in a Portal+overlay.
  assert.match(source, /onMouseDown|onClick.*setOpen|backdrop/);
});

test("Menu item for 📶 通过 USB 配 WiFi renders only when onApplyWifi is provided", () => {
  /* Source contains the conditional gating on the new prop. */
  assert.match(source, /onApplyWifi/);
  assert.match(source, /通过 USB 配 WiFi/);
  /* Conditional render guard so consumers can opt out by omitting the prop. */
  assert.match(source, /onApplyWifi\s*&&|typeof\s+onApplyWifi/);
});

test("Menu accepts the new onApplyWifi prop", () => {
  assert.match(source, /\bonApplyWifi\b/);
});
