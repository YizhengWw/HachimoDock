/**
 * [Input] Read PageShell.jsx source.
 * [Output] Static Node coverage that PageShell renders title, optional subtitle/actions/help with documented class names and prop signature.
 * [Pos] test node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "PageShell.jsx"), "utf8");

test("PageShell exports a default React component", () => {
  assert.match(source, /export default function PageShell\s*\(/);
});

test("PageShell accepts the documented props", () => {
  for (const prop of ["title", "subtitle", "actions", "help", "children"]) {
    assert.match(source, new RegExp(`\\b${prop}\\b`), `expected prop ${prop} in PageShell`);
  }
});

test("PageShell uses the documented class hierarchy", () => {
  assert.match(source, /className="page-shell"/);
  assert.match(source, /className="page-shell__header"/);
  assert.match(source, /className="page-shell__title"/);
});

test("PageShell only renders subtitle when provided", () => {
  // Conditional render guard — keeps the header tight when subtitle is omitted.
  assert.match(source, /subtitle\s*&&/);
});

test("PageShell only renders the help icon when help is passed", () => {
  assert.match(source, /help\s*&&/);
  assert.match(source, /HelpCircle/);
});

test("PageShell renders the actions slot at the top-right", () => {
  assert.match(source, /className="page-shell__actions"/);
  // actions is rendered as-is (caller passes a node or array)
  assert.match(source, /\{actions\}/);
});
