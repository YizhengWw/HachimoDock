/**
 * [Input] firmware-version.js pure helpers.
 * [Output] Regression coverage for older, equal, newer, suffixed, and invalid versions.
 * [Pos] test node in ref/src/lib.
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  compareFirmwareVersions,
  firmwareUpdateDisposition,
  parseFirmwareVersion,
} from "./firmware-version.js";

test("firmware versions compare numerically and ignore product suffixes", () => {
  assert.deepEqual(parseFirmwareVersion("v0.7.29-p4"), [0, 7, 29]);
  assert.equal(compareFirmwareVersions("0.7.9-p4", "0.7.29-p4"), -1);
  assert.equal(compareFirmwareVersions("0.7.29", "0.7.29-p4"), 0);
  assert.equal(compareFirmwareVersions("0.8.0-p4", "0.7.29-p4"), 1);
});

test("firmware update disposition exposes update only for older devices", () => {
  assert.equal(firmwareUpdateDisposition("0.7.28-p4", "0.7.29-p4"), "update");
  assert.equal(firmwareUpdateDisposition("0.7.29-p4", "0.7.29-p4"), "latest");
  assert.equal(firmwareUpdateDisposition("0.7.30-p4", "0.7.29-p4"), "latest");
  assert.equal(firmwareUpdateDisposition("unknown", "0.7.29-p4"), "unknown");
});
