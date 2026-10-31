/**
 * [Input] WifiSettings.jsx, ApiSettings.jsx, the Rust usb_send_topic command,
 *         and the local-only Tauri permission allowlist.
 * [Output] Static Node coverage for USB-based Wi-Fi provisioning topics, password
 *          non-persistence, 2.4GHz guidance, and settings-page wiring.
 * [Pos] test node in pc/src
 * [Sync] If this file changes, update pc/src/.folder.md.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));
const tauriDir = join(srcDir, "..", "src-tauri");

function read(fileName) {
  return readFileSync(join(srcDir, fileName), "utf8");
}

test("Wi-Fi settings uses the optional USB provisioning topics", () => {
  const source = read("WifiSettings.jsx");
  for (const topic of ["wifi/status", "wifi/scan", "wifi/config", "wifi/reset"]) {
    assert.match(source, new RegExp(topic.replace("/", "\\/")));
  }
  assert.match(source, /usb_send_topic/);
  assert.match(source, /Wi-Fi TCP 通道暂不可用/);
  assert.match(source, /C6/);
  assert.match(source, /c6Version/);
  assert.match(source, /wifiTcpCompatible/);
  assert.match(source, /2\.4GHz/);
  assert.match(source, /type="password"/);
});

test("Wi-Fi password is never persisted by the renderer", () => {
  const source = read("WifiSettings.jsx");
  assert.doesNotMatch(source, /localStorage/);
  assert.doesNotMatch(source, /sessionStorage/);
  assert.doesNotMatch(source, /password:\s*password\s*[,}]/);
});

test("Wi-Fi settings is reachable from the device tab", () => {
  const dashboard = read("DeviceDashboard.jsx");
  const statusBar = read("dashboard/DeviceStatusBar.jsx");
  assert.match(dashboard, /import WifiSettings from "\.\/WifiSettings\.jsx"/);
  assert.match(dashboard, /<WifiSettings \/>/);
  assert.match(dashboard, /<DeviceStatusBar onOpenWifi=/);
  assert.match(statusBar, /通过 Wi-Fi 连接/);
  assert.match(statusBar, /onOpenWifi/);
});

test("usb_send_topic is registered and local-only", () => {
  const rust = readFileSync(join(tauriDir, "src", "lib.rs"), "utf8");
  assert.match(rust, /async fn usb_send_topic\(/);
  assert.match(rust, /usb_manager\.send\(&topic, &payload\)/);
  const permission = JSON.parse(readFileSync(join(tauriDir, "permissions", "local-app-commands.json"), "utf8"));
  assert.ok(permission.permission[0].commands.allow.includes("usb_send_topic"));
});
