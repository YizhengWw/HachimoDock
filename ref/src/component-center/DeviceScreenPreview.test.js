/**
 * [Input] Read DeviceScreenPreview.jsx source.
 * [Output] Static source coverage: default export, stable slot layout, cds-title-badge, cds-progress, normalizeProgress guard.
 * [Pos] test node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "DeviceScreenPreview.jsx"), "utf8");
const styles = readFileSync(join(here, "../styles.css"), "utf8");

// 1. Default export
test("DeviceScreenPreview exports a default React function", () => {
  assert.match(source, /export default function DeviceScreenPreview\s*\(/);
});

// 2. Renders cds-title-badge from dashboard.title
test("DeviceScreenPreview renders cds-title-badge using dashboard.title", () => {
  assert.match(source, /cds-title-badge/);
  assert.match(source, /dashboard\.title/);
});

// 3. Renders progress bar
test("DeviceScreenPreview renders cds-progress and cds-progress__bar", () => {
  assert.match(source, /cds-progress/);
  assert.match(source, /cds-progress__bar/);
});

test("DeviceScreenPreview keeps progress in a separate bottom slot", () => {
  const panelClose = source.indexOf("      )}");
  const progressSlot = source.indexOf("      {progress && (");
  assert.notEqual(panelClose, -1);
  assert.notEqual(progressSlot, -1);
  assert.ok(progressSlot > panelClose);
});

// 4. Guards against bad progress values via normalizeProgress
test("DeviceScreenPreview normalizes progress and clamps to 0-100", () => {
  assert.match(source, /normalizeProgress/);
  assert.match(source, /Math\.max\(0, Math\.min\(100/);
});

// 5. Wraps everything in component-device-screen
test("DeviceScreenPreview uses component-device-screen as root class", () => {
  assert.match(source, /component-device-screen/);
  assert.match(source, /data-widget=\{component\.id\}/);
});

test("device screen and candidate preview styles reserve stable frame slots", () => {
  assert.match(styles, /\.component-device-screen\s*\{[\s\S]*grid-template-rows:/);
  assert.match(styles, /\.candidate-card__preview\s*\{[\s\S]*align-items:\s*center/);
  assert.match(styles, /\.candidate-card__preview\s*\{[\s\S]*min-height:/);
});
