import test from "node:test";
import assert from "node:assert/strict";
import {
  normalizeTextRenderingSettings,
  textRenderingModeForTransport,
} from "./text-rendering-settings.js";

test("text rendering defaults use the app for USB and the client for Wi-Fi", () => {
  assert.deepEqual(normalizeTextRenderingSettings({}), { usbApp: true, wifiApp: false });
  assert.equal(textRenderingModeForTransport("usb", {}), "app");
  assert.equal(textRenderingModeForTransport("wifi", {}), "client");
});

test("text rendering settings accept explicit transport overrides", () => {
  assert.equal(textRenderingModeForTransport("usb", { usbApp: false }), "client");
  assert.equal(textRenderingModeForTransport("wifi", { wifiApp: true }), "app");
});
