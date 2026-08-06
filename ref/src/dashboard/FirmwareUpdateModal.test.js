/**
 * [Input] FirmwareUpdateModal.jsx and shared styles source.
 * [Output] Static coverage for native .bin selection, ACK-gated Tauri update,
 *          progress stages, reboot result, and stable modal geometry.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "FirmwareUpdateModal.jsx"), "utf8");
const styles = readFileSync(join(here, "..", "styles.css"), "utf8");

test("firmware modal selects only ESP32-P4 .bin images", () => {
  assert.match(source, /openDialog/);
  assert.match(source, /extensions:\s*\["bin"\]/);
  assert.match(source, /ESP32-P4 firmware/);
});

test("firmware modal invokes the ACK-gated backend and listens for progress", () => {
  assert.match(source, /invoke\("usb_update_firmware",\s*\{[\s\S]*?firmwarePath,[\s\S]*?expectedBoardDeviceId,[\s\S]*?\}\)/);
  assert.match(source, /listen\("usb-firmware-update-progress"/);
  for (const stage of ["begin", "recover", "upload", "verify", "reboot", "validate"]) {
    assert.match(source, new RegExp(`\\b${stage}:`));
  }
});

test("firmware modal exposes only backend-validated version, partition and image state", () => {
  assert.match(source, /result\.version/);
  assert.match(source, /result\.targetPartition/);
  assert.match(source, /result\.imageState/);
  assert.match(source, /设备已重连/);
  assert.match(source, /确认有效/);
});

test("firmware modal describes integrity validation without claiming a signature", () => {
  assert.match(source, /镜像完整性/);
  assert.doesNotMatch(source, /签名/);
});

test("firmware modal has bounded responsive dimensions and stable progress geometry", () => {
  assert.match(styles, /\.firmware-update-modal\s*\{[\s\S]*width:\s*min\(540px/);
  assert.match(styles, /\.firmware-update-modal__progress-meta\s*\{[\s\S]*grid-template-columns:/);
  assert.match(styles, /\.firmware-update-modal__progress-track\s*\{[\s\S]*height:\s*6px/);
});
