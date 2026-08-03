/**
 * [Input] Read shared Switch.jsx and global styles.
 * [Output] Static coverage for unified accessible switch state, callbacks, labels, and visual contract.
 * [Pos] test node in ref/src/shell.
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "Switch.jsx"), "utf8");
const css = readFileSync(join(here, "..", "styles.css"), "utf8");

test("Switch owns accessible native switch semantics and checked callbacks", () => {
  assert.match(source, /type="checkbox"/);
  assert.match(source, /role="switch"/);
  assert.match(source, /checked=\{Boolean\(checked\)\}/);
  assert.match(source, /disabled=\{disabled\}/);
  assert.match(source, /onCheckedChange\?\.\(event\.target\.checked\)/);
  assert.match(source, /aria-label=\{ariaLabel/);
});

test("Switch exposes one shared track, thumb, label, focus, and checked style", () => {
  assert.match(source, /switch__track/);
  assert.match(source, /switch__thumb/);
  assert.match(source, /switch__label/);
  assert.match(css, /\.switch\s*{/);
  assert.match(css, /\.switch__track\s*{/);
  assert.match(css, /\.switch input:checked \+ \.switch__track\s*{/);
  assert.match(css, /\.switch input:focus-visible \+ \.switch__track\s*{/);
});
