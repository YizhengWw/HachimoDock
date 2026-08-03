/**
 * [Input] DeviceDiagnosticsModal.jsx and shared styles.
 * [Output] Static coverage for ACK-gated diagnostics, crash/reset metrics,
 *          explicit confirmations, and asset-preserving recovery copy.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "DeviceDiagnosticsModal.jsx"), "utf8");
const styles = readFileSync(join(here, "..", "styles.css"), "utf8");

test("diagnostics modal uses ACK-gated P4 commands", () => {
  for (const command of ["usb_get_diagnostics", "usb_reset_input_config", "usb_reboot_device"]) {
    assert.match(
      source,
      new RegExp(`invoke\\(\"${command}\",\\s*\\{ expectedBoardDeviceId \\}\\)`),
    );
  }
  assert.match(source, /expectedBoardDeviceId/);
});

test("diagnostics modal exposes reset, memory, storage and input health", () => {
  for (const field of ["lastResetReason", "faultResetCount", "minimumFreeHeapBytes", "freePsramBytes", "activeAppearanceSlot", "inputDroppedEvents", "imageState"]) {
    assert.match(source, new RegExp(field));
  }
});

test("destructive-looking recovery actions require a second click and preserve assets", () => {
  assert.match(source, /再次点击确认/);
  assert.match(source, /保留设备中的形象素材/);
});

test("diagnostics modal has bounded responsive geometry", () => {
  assert.match(styles, /\.device-diagnostics-modal\s*\{[\s\S]*width:\s*min\(620px/);
  assert.match(styles, /\.device-diagnostics-modal__metrics\s*\{[\s\S]*grid-template-columns:/);
});
