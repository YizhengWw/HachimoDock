/**
 * [Input] wifi-link.js helpers with deterministic status snapshots.
 * [Output] Behavior coverage for credential validation, link labels, status text and error codes.
 * [Pos] test node in pc/src/lib
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  canProvisionWifi,
  describeWifiStatus,
  isWifiLink,
  linkLabel,
  validateWifiCredentials,
  wifiAddressFromPortName,
  wifiErrorMessage,
  wifiSupported,
} from "./wifi-link.js";

test("credential rules match the firmware and Rust validators", () => {
  assert.equal(validateWifiCredentials("Home", "correct horse"), "");
  assert.equal(validateWifiCredentials("Cafe", ""), "");
  assert.equal(validateWifiCredentials("a".repeat(32), "12345678"), "");
  assert.notEqual(validateWifiCredentials("", "12345678"), "");
  assert.notEqual(validateWifiCredentials("a".repeat(33), "12345678"), "");
  assert.notEqual(validateWifiCredentials("Home", "short"), "");
  assert.notEqual(validateWifiCredentials("Home", "1234567"), "");
  assert.equal(validateWifiCredentials("Home", "x".repeat(63)), "");
  assert.notEqual(validateWifiCredentials("Home", "x".repeat(64)), "");
  assert.equal(validateWifiCredentials("Home", "F".repeat(64)), "");
});

test("SSID length is counted in UTF-8 bytes, like the radio does", () => {
  assert.equal(validateWifiCredentials("网".repeat(10), "12345678"), "", "30 bytes fits");
  assert.notEqual(validateWifiCredentials("网".repeat(11), "12345678"), "", "33 bytes does not");
});

test("link labels distinguish USB and WiFi and never claim a link that is down", () => {
  assert.equal(linkLabel({ connected: false, transport: "wifi" }), "未连接");
  assert.equal(linkLabel({ connected: true, transport: "usb" }), "USB 直连");
  assert.equal(linkLabel({ connected: true }), "USB 直连");
  assert.equal(linkLabel({ connected: true, transport: "wifi" }), "WiFi 无线");
  assert.equal(isWifiLink({ connected: false, transport: "wifi" }), false);
  assert.equal(wifiAddressFromPortName("wifi://192.168.1.9:38517"), "192.168.1.9:38517");
  assert.equal(wifiAddressFromPortName("/dev/cu.usbmodem1"), "");
});

test("provisioning needs the USB cable and firmware that advertises WiFi", () => {
  const capable = { connected: true, transport: "usb", capabilities: { network: { wifi: true } } };
  assert.equal(canProvisionWifi(capable), true);
  assert.equal(canProvisionWifi({ ...capable, transport: "wifi" }), false);
  assert.equal(canProvisionWifi({ ...capable, connected: false }), false);
  assert.equal(canProvisionWifi({ connected: true, transport: "usb", capabilities: {} }), false);
  assert.equal(wifiSupported(null), false);
});

test("status text covers every state and surfaces actionable errors", () => {
  assert.equal(describeWifiStatus(null).tone, "muted");
  assert.equal(describeWifiStatus({ supported: false }).tone, "warning");
  assert.equal(describeWifiStatus({ state: "unconfigured" }).text, "尚未配置 WiFi");
  assert.equal(describeWifiStatus({ state: "connecting" }).tone, "pending");
  const connected = describeWifiStatus({ state: "connected", ssid: "Home", ip: "192.168.1.9", rssi: -52, client: true });
  assert.equal(connected.tone, "success");
  assert.match(connected.text, /Home/);
  assert.match(connected.text, /192\.168\.1\.9/);
  assert.match(connected.text, /-52 dBm/);
  assert.match(connected.text, /电脑已通过 WiFi 连接/);
  const wrong = describeWifiStatus({ state: "failed", lastError: "wrong_password" });
  assert.equal(wrong.tone, "error");
  assert.match(wrong.text, /密码/);
  assert.match(describeWifiStatus({ state: "failed", lastError: "crash_loop" }).text, /重新保存/);
  assert.match(describeWifiStatus({ state: "failed", lastError: "ap_not_found" }).text, /找不到/);
});

test("unknown error codes are shown rather than hidden", () => {
  assert.equal(wifiErrorMessage(""), "");
  assert.match(wifiErrorMessage("brand_new_error"), /brand_new_error/);
});
