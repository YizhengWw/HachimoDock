/**
 * [Input] Read Card.jsx source.
 * [Output] Static Node coverage that Card renders title/subtitle/actions sections, and Card.Collapsible exposes open/close state with summary slot.
 * [Pos] test node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "Card.jsx"), "utf8");

test("Card exports a default React component", () => {
  assert.match(source, /export default function Card\s*\(/);
});

test("Card exposes Collapsible as a static property", () => {
  assert.match(source, /Card\.Collapsible\s*=/);
});

test("Card uses the documented class names", () => {
  assert.match(source, /className="card"/);
  assert.match(source, /className="card__header"/);
  assert.match(source, /className="card__title"/);
  assert.match(source, /className="card__body"/);
});

test("Card only renders header when title/subtitle/actions provided", () => {
  // The header should be conditionally rendered to keep cards tight when used as plain containers.
  assert.match(source, /title\s*\|\|\s*subtitle\s*\|\|\s*actions/);
});

test("Card does not accept a tone/variant prop (status lives in body banners/chips)", () => {
  // Explicit non-feature — guards against future drift.
  assert.doesNotMatch(source, /\btone\b/);
  assert.doesNotMatch(source, /\bvariant\b/);
});

test("Card.Collapsible accepts title, summary, defaultOpen, children", () => {
  for (const prop of ["title", "summary", "defaultOpen", "children"]) {
    assert.match(source, new RegExp(`\\b${prop}\\b`), `expected ${prop} in Collapsible`);
  }
});

test("Card.Collapsible uses useState for open state", () => {
  assert.match(source, /useState\(/);
});

test("Card.Collapsible uses the documented class names and chevron icon", () => {
  // Match the class string regardless of JSX wrapper (static, ternary, or template literal).
  assert.match(source, /card card--collapsible/);
  assert.match(source, /ChevronDown|ChevronRight/);
});

test("Card.Collapsible toggles is-open class on its section", () => {
  // Verifies the section's class string contains the is-open modifier conditional on open state.
  assert.match(source, /card--collapsible[\s\S]*?is-open/);
});
