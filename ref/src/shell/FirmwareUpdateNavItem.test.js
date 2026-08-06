/**
 * [Input] FirmwareUpdateNavItem.jsx and App.jsx source.
 * [Output] Static coverage for sidebar placement, version-gated Update/Up-to-date
 *          states, bundled OTA command, progress, and no detail-page routing.
 * [Pos] test node in ref/src/shell.
 * [Sync] If this file changes, update `ref/src/shell/.folder.md` and `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "FirmwareUpdateNavItem.jsx"), "utf8");
const app = readFileSync(join(here, "..", "App.jsx"), "utf8");

test("firmware module sits directly below API configuration without a page route", () => {
  assert.match(app, /title="API 配置"[\s\S]*?<FirmwareUpdateNavItem usb=\{usb\} \/>/);
  assert.doesNotMatch(app, /view === "firmware"|setView\("firmware"\)/);
});

test("firmware module exposes Update only for older devices and Up to date otherwise", () => {
  assert.match(source, /firmwareUpdateDisposition\(currentVersion, bundledFirmware\?\.version\)/);
  assert.match(source, /disposition === "update"[\s\S]*"更新"/);
  assert.match(source, /disposition === "latest"[\s\S]*已最新/);
  assert.match(source, /usb_get_bundled_firmware_info/);
  assert.match(source, /usb_update_bundled_firmware/);
  assert.match(source, /usb-firmware-update-progress/);
});
