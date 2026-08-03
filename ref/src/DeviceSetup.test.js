/**
 * [Input] DeviceSetup.jsx and App.jsx first-run routing sources.
 * [Output] Static Node coverage for USB-only automatic discovery, binding persistence, manual rescan, and removal of the legacy Ethernet/Wi-Fi wizard.
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

test("first-run setup is USB-only and contains no legacy network wizard", () => {
  const setup = readSource("DeviceSetup.jsx");
  const app = readSource("App.jsx");
  const combined = `${setup}\n${app}`;

  assert.match(setup, /USB 自动连接/);
  assert.match(setup, /连接你的桌宠/);
  assert.doesNotMatch(combined, /插网线绑定|Wi-Fi 绑定|Wi‑Fi 绑定|网络绑定|验证通信|确认形象/);
  assert.doesNotMatch(
    setup,
    /wifi_get_status|wifi_connect_ap|device_get_pairing_state|device_get_wifi_scan|device_apply_config|wifi_restore/,
  );
});

test("setup automatically probes verified USB devices and keeps polling", () => {
  const source = readSource("DeviceSetup.jsx");

  assert.match(source, /invoke\("usb_get_status"\)/);
  assert.match(source, /invoke\("usb_scan_devices"\)/);
  assert.match(source, /invoke\("usb_connect", \{ portName \}\)/);
  assert.match(source, /status\.connected && status\.boardDeviceId/);
  assert.match(source, /window\.setInterval/);
  assert.match(source, /AUTO_SCAN_INTERVAL_MS/);
});

test("verified USB identity is persisted before entering the dashboard", () => {
  const source = readSource("DeviceSetup.jsx");

  assert.match(source, /invoke\("get_or_create_desktop_device_id"\)/);
  assert.match(source, /invoke\("save_device_binding"/);
  assert.match(source, /boardDeviceId: status\.boardDeviceId/);
  assert.match(source, /wifiSsid: `USB\(/);
  assert.match(source, /await finish\(\)/);
});

test("setup exposes one bounded manual recovery action", () => {
  const source = readSource("DeviceSetup.jsx");

  assert.match(source, /重新扫描/);
  assert.match(source, /onClick=\{\(\) => scan\(\)\}/);
  assert.match(source, /disabled=\{busy\}/);
  assert.match(source, /使用可传输数据的 USB 线连接设备/);
});

test("setup completion refreshes shared device context before dashboard render", () => {
  const source = readSource("App.jsx");

  assert.match(source, /useDeviceContext/);
  assert.match(source, /const\s+\{\s*refresh\s*\}\s*=\s*useDeviceContext\(\)/);
  assert.match(source, /const\s+handleSetupCompleteWithRefresh\s*=\s*useCallback\(\s*async\s*\(\)\s*=>/);
  assert.match(source, /await\s+refresh\(\)/);
  assert.match(source, /<DeviceSetup\s+onComplete=\{handleSetupCompleteWithRefresh\}/);
});
