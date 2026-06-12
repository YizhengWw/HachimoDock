/**
 * [Input] src-tauri/src/usb_serial.rs widget OTA transport.
 * [Output] Static regression coverage that widget install cannot report success
 *          when the board never acknowledges begin/commit.
 * [Pos] test node in ref/src
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));
const usbSerialSource = readFileSync(join(srcDir, "../src-tauri/src/usb_serial.rs"), "utf8");

test("USB widget OTA treats missing begin ack as an install failure", () => {
  assert.doesNotMatch(usbSerialSource, /assuming legacy board, continuing/);
  assert.match(usbSerialSource, /未收到板端组件 OTA 确认:[\s\S]*phase=\{\}/);
});

test("USB widget OTA treats missing commit ack as an install failure", () => {
  assert.doesNotMatch(usbSerialSource, /returning success/);
  assert.match(usbSerialSource, /未收到板端组件 OTA 确认:[\s\S]*phase=\{\}/);
});
