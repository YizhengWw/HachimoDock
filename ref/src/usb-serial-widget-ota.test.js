/**
 * [Input] src-tauri/src/usb_serial.rs USB OTA transports.
 * [Output] Static regression coverage that widget install cannot report success
 *          when the board never acknowledges begin/commit, and appearance
 *          asset OTA is serialized before entering the board's global staging,
 *          while board asset commit triggers a one-shot welcome replay.
 * [Pos] test node in ref/src
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));
const usbSerialSource = readFileSync(join(srcDir, "../src-tauri/src/usb_serial.rs"), "utf8");
const boardSerialBridgeSource = readFileSync(join(srcDir, "../../board-runtime/src/board_serial_bridge.c"), "utf8");

test("USB widget OTA treats missing begin ack as an install failure", () => {
  assert.doesNotMatch(usbSerialSource, /assuming legacy board, continuing/);
  assert.match(usbSerialSource, /未收到板端组件 OTA 确认:[\s\S]*phase=\{\}/);
});

test("USB widget OTA treats missing commit ack as an install failure", () => {
  assert.doesNotMatch(usbSerialSource, /returning success/);
  assert.match(usbSerialSource, /未收到板端组件 OTA 确认:[\s\S]*phase=\{\}/);
});

test("appearance asset OTA is serialized across full sync and audio patch", () => {
  assert.match(usbSerialSource, /asset_transfer_guard:\s*Arc<Mutex<\(\)>>/);
  assert.match(usbSerialSource, /asset_transfer_guard:\s*Arc::new\(Mutex::new\(\(\)\)\)/);
  assert.match(
    usbSerialSource,
    /let\s+_asset_transfer_guard\s*=\s*self[\s\S]*?\.asset_transfer_guard[\s\S]*?\.lock\(\)/,
  );
  const syncBody = usbSerialSource.match(/pub fn sync_appearance<F>\([\s\S]*?let manifest_path =/);
  assert.ok(syncBody, "expected sync_appearance body");
  assert.match(syncBody[0], /_asset_transfer_guard/);
});

test("board appearance asset commit emits a one-shot welcome trigger", () => {
  const commitBody = boardSerialBridgeSource.match(/static void br_serial_handle_asset_commit[\s\S]*?\n}/);
  assert.ok(commitBody, "expected br_serial_handle_asset_commit");
  assert.match(commitBody[0], /br_atomic_write_text\([^)]*welcome_trigger_path[^)]*marker\)/);
});
