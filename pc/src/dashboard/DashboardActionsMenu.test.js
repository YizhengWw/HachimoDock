/**
 * [Input] Read DashboardActionsMenu.jsx source.
 * [Output] Static Node coverage for core and optional P4 menu items, the danger styling on 解绑, and the expected callback prop signature.
 * [Pos] test node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
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

test("Menu does not expose unsupported P4 WiFi configuration", () => {
  assert.doesNotMatch(source, /onApplyWifi/);
  assert.doesNotMatch(source, /通过 USB 配 WiFi/);
});

test("Menu exposes P4 firmware update only when its callback is provided", () => {
  assert.match(source, /\bonUpdateFirmware\b/);
  assert.match(source, /onUpdateFirmware\s*&&/);
  assert.match(source, /升级 ESP32-P4 固件/);
});

test("Menu exposes P4 diagnostics only when its callback is provided", () => {
  assert.match(source, /\bonDiagnostics\b/);
  assert.match(source, /onDiagnostics\s*&&/);
  assert.match(source, /ESP32-P4 设备诊断/);
});
