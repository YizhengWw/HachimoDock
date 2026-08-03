/**
 * [Input] usb_serial execution, transaction waiters, and widget policy.
 * [Output] Static regression coverage that widget install cannot report success
 *          when the board never acknowledges begin/chunk/commit, with per-file
 *          integrity checks, exact ACK identity, bounded retry, and serialized
 *          widget/appearance asset OTA before entering board staging, including
 *          host cancellation and board-side appearance staging cleanup,
 *          while board asset commit triggers a one-shot welcome replay and
 *          agent state forwarding keeps USB as primary with SSH fallback.
 * [Pos] test node in ref/src
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));
const usbSerialSource = [
  readFileSync(join(srcDir, "../src-tauri/src/usb_serial.rs"), "utf8"),
  readFileSync(
    join(srcDir, "../src-tauri/src/usb_serial/widget_transaction.rs"),
    "utf8",
  ),
  readFileSync(
    join(srcDir, "../src-tauri/src/usb_serial/transaction_waiters.rs"),
    "utf8",
  ),
].join("\n");
const tauriSource = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
const boardSerialBridgeSource = readFileSync(join(srcDir, "../../legacy/board-runtime/src/board_serial_bridge.c"), "utf8");
const boardServerSource = readFileSync(join(srcDir, "../../legacy/board-runtime/src/board_server.c"), "utf8");
const p4ProtocolSource = readFileSync(join(srcDir, "../../esp-p4-runtime/main/pet_p4_protocol.c"), "utf8");
const p4NativeUsbSource = readFileSync(join(srcDir, "../../esp-p4-runtime/main/pet_p4_usb_native.c"), "utf8");

test("USB widget OTA treats missing begin ack as an install failure", () => {
  assert.doesNotMatch(usbSerialSource, /assuming legacy board, continuing/);
  assert.match(usbSerialSource, /fn format_widget_ack_timeout\(transfer_id: &str, phase: &str\) -> String/);
  assert.match(usbSerialSource, /format!\([\s\S]*transferId=\{\} phase=\{\}/);
});

test("USB widget OTA treats missing commit ack as an install failure", () => {
  assert.doesNotMatch(usbSerialSource, /returning success/);
  assert.match(usbSerialSource, /fn format_widget_ack_timeout\(transfer_id: &str, phase: &str\) -> String/);
  assert.match(usbSerialSource, /format!\([\s\S]*transferId=\{\} phase=\{\}/);
});

test("widget commit and delete keep ACK waiters through slow P4 flash sync", () => {
  assert.match(
    usbSerialSource,
    /const WIDGET_COMMIT_ACK_TIMEOUT: Duration = Duration::from_secs\(15\);/,
  );
  assert.match(
    usbSerialSource,
    /const WIDGET_DELETE_ACK_TIMEOUT: Duration = Duration::from_secs\(15\);/,
  );
});

test("USB widget OTA checksum-verifies and retries every file chunk", () => {
  assert.match(usbSerialSource, /const WIDGET_CHUNK_MAX_ATTEMPTS: usize = 3/);
  assert.match(
    usbSerialSource,
    /register_widget_ack_waiter\(&transfer_id, "chunk", Some\(rel\), Some\(0\)\)/,
  );
  assert.match(usbSerialSource, /chunk_rx\.recv_timeout\(WIDGET_CHUNK_ACK_TIMEOUT\)/);
  assert.match(usbSerialSource, /"decodedSize": decoded_size/);
  assert.match(usbSerialSource, /"checksum": checksum/);
  assert.match(usbSerialSource, /self\.send\("widget\/chunk", &payload\)/);
  assert.doesNotMatch(usbSerialSource, /send_no_flush\("widget\/chunk"/);
  assert.match(usbSerialSource, /waiter\.path\.as_deref\(\) == path/);
  assert.match(usbSerialSource, /waiter\.index == index/);
  const installBody = usbSerialSource.match(
    /pub fn install_widget_clawpkg<F>\([\s\S]*?let transfer_id = format!\(/,
  );
  assert.ok(installBody, "expected install_widget_clawpkg body");
  assert.match(installBody[0], /asset_transfer_guard[\s\S]*?\.lock\(\)/);
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

test("appearance USB sync can be cancelled and both device runtimes discard staging", () => {
  assert.match(usbSerialSource, /appearance_sync_cancel_requested:\s*Arc<AtomicBool>/);
  assert.match(usbSerialSource, /pub fn cancel_appearance_sync\(&self\) -> bool/);
  assert.match(usbSerialSource, /best_effort_asset_abort/);
  assert.match(usbSerialSource, /"asset\/abort"/);
  assert.match(usbSerialSource, /Older P4 firmware predates asset\/abort/);
  assert.match(usbSerialSource, /"fileCount": 0/);
  assert.match(tauriSource, /fn usb_cancel_appearance_sync\(/);
  assert.match(tauriSource, /usb_cancel_appearance_sync,\s*usb_sync_appearance/);
  assert.match(boardServerSource, /br_handle_asset_abort/);
  assert.match(boardServerSource, /br_asset_remove_tree\(staging\)/);
  assert.match(p4ProtocolSource, /strcmp\(topic, "asset\/abort"\) == 0/);
  assert.match(p4ProtocolSource, /state->asset_transfer_active = false/);
  assert.match(p4NativeUsbSource, /handle_asset_abort_json/);
  assert.match(p4NativeUsbSource, /g_target_slot = -1/);
});

test("board appearance asset commit emits a one-shot welcome trigger", () => {
  const commitBody = boardSerialBridgeSource.match(/static void br_serial_handle_asset_commit[\s\S]*?\n}/);
  assert.ok(commitBody, "expected br_serial_handle_asset_commit");
  assert.match(commitBody[0], /br_atomic_write_text\([^)]*welcome_trigger_path[^)]*marker\)/);
});

test("agent state forwarding is USB-first with SSH fallback only when disconnected", () => {
  assert.match(tauriSource, /fn send_state_via_ssh_fallback/);
  assert.match(tauriSource, /PET_MANAGER_STATE_FALLBACK_SSH_HOST/);
  const forwarderBody = tauriSource.match(/fn start_usb_state_forwarder\([\s\S]*?fn start_usb_auto_connect/);
  assert.ok(forwarderBody, "expected start_usb_state_forwarder body");
  const disconnectedBlock = forwarderBody[0].match(
    /if !status\.connected \{[\s\S]*?\r?\n\s+continue;\r?\n\s+\}/,
  );
  assert.ok(disconnectedBlock, "expected disconnected branch in state forwarder");
  assert.match(disconnectedBlock[0], /send_state_via_ssh_fallback/);
  assert.doesNotMatch(disconnectedBlock[0], /usb_manager\.send_state/);
});
