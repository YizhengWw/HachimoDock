/**
 * [Input] Read ContextRail.jsx source.
 * [Output] Static Node coverage that ContextRail renders the bound triad (device/appearance/component) with navigation callbacks, and collapses to a single bind CTA when no binding.
 * [Pos] test node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "ContextRail.jsx"), "utf8");

test("ContextRail exports a default React component", () => {
  assert.match(source, /export default function ContextRail\s*\(/);
});

test("ContextRail reads its data from useDeviceContext (single source)", () => {
  assert.match(source, /useDeviceContext\(/);
});

test("ContextRail accepts navigation callbacks for the three rows", () => {
  for (const cb of ["onOpenDevice", "onOpenAppearance", "onOpenComponent", "onStartBinding"]) {
    assert.match(source, new RegExp(`\\b${cb}\\b`), `expected ${cb} callback`);
  }
});

test("ContextRail uses the documented class names", () => {
  assert.match(source, /className="context-rail"/);
  assert.match(source, /context-rail__row/);
});

test("ContextRail collapses to a bind CTA when no binding", () => {
  // The branch must visibly include the bind label and call onStartBinding.
  assert.match(source, /绑定设备/);
});
