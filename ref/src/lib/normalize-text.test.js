/**
 * [Input] `normalizeText` from `./normalize-text.js`.
 * [Output] Behavior coverage (bare node) for trimming, the empty→fallback path,
 *          and nullish coercion.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { normalizeText } from "./normalize-text.js";

test("normalizeText trims the value to a string", () => {
  assert.equal(normalizeText("  hello  "), "hello");
  assert.equal(normalizeText(42), "42");
});

test("normalizeText falls back to the trimmed fallback when empty", () => {
  assert.equal(normalizeText("   ", "fallback"), "fallback");
  assert.equal(normalizeText(null, "  fb  "), "fb");
  assert.equal(normalizeText(undefined), "");
});
