/**
 * [Input] Read DeviceStatusBar.jsx source.
 * [Output] Static Node coverage that the status bar reads from useDeviceContext, renders board id + separate USB/WiFi status chips, exposes manual USB serial rescan, and uses the documented class names.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "DeviceStatusBar.jsx"), "utf8");

test("DeviceStatusBar exports a default React component", () => {
  assert.match(source, /export default function DeviceStatusBar\s*\(/);
});

test("DeviceStatusBar consumes useDeviceContext (no local polling)", () => {
  assert.match(source, /useDeviceContext\(/);
  assert.doesNotMatch(source, /invoke\(/);
  assert.doesNotMatch(source, /usb_get_status/);
  assert.doesNotMatch(source, /check_device_availability/);
});

test("DeviceStatusBar exposes a manual USB serial rescan button", () => {
  assert.match(source, /rescanUsbDevices/);
  assert.match(source, /重新扫描串口/);
  assert.match(source, /handleRescanUsb/);
});

test("DeviceStatusBar renders USB and WiFi as independent states", () => {
  assert.match(source, /wifiOnline/);
  assert.match(source, /usbChip/);
  assert.match(source, /wifiChip/);
  assert.match(source, /USB 直连/);
  assert.match(source, /WiFi 在线/);
  assert.match(source, /WiFi 离线/);
  assert.match(source, /dashboard-status-bar__chips/);
  assert.doesNotMatch(source, /else if \(deviceOnline\)/);
});

test("DeviceStatusBar reads binding.boardDeviceId and binding.wifiSsid", () => {
  assert.match(source, /binding\.boardDeviceId/);
  assert.match(source, /binding\.wifiSsid/);
});

test("DeviceStatusBar uses the documented class names", () => {
  assert.match(source, /className="dashboard-status-bar/);
  assert.match(source, /dashboard-status-bar__chip/);
  assert.match(source, /dashboard-status-bar__scan-btn/);
});
