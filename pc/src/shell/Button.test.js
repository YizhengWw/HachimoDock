/**
 * [Input] Read shell/Button.jsx and the shared styles.
 * [Output] Static coverage for semantic variants, sizes, loading/disabled behavior,
 *          and the token-driven shared button contract.
 * [Pos] Test node in pc/src/shell.
 * [Sync] If this file changes, update `pc/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "Button.jsx"), "utf8");
const styles = readFileSync(join(here, "..", "styles.css"), "utf8");

test("Button exposes the shared semantic variants and control sizes", () => {
  assert.match(source, /primary.*secondary.*ghost.*danger.*icon/);
  assert.match(source, /small.*medium.*large/);
  assert.match(source, /button--\$\{resolvedVariant\}/);
  assert.match(source, /button--\$\{resolvedSize\}/);
});

test("Button owns native type, disabled, and loading semantics", () => {
  assert.match(source, /type = "button"/);
  assert.match(source, /disabled=\{disabled \|\| loading\}/);
  assert.match(source, /aria-busy=\{loading \|\| undefined\}/);
  assert.match(source, /button__spinner/);
  assert.match(source, /button__loading/);
  assert.match(source, /aria-hidden=\{loading \|\| undefined\}/);
  assert.match(source, /loadingLabel/);
});

test("shared button CSS defines hierarchy, active feedback, icon control, and reduced motion", () => {
  assert.match(styles, /\.button--primary/);
  assert.match(styles, /\.button--secondary/);
  assert.match(styles, /\.button--ghost/);
  assert.match(styles, /\.button--danger/);
  assert.match(styles, /\.button--icon/);
  assert.match(styles, /\.button:active:not\(:disabled\)/);
  assert.match(styles, /\.button__spinner/);
  assert.match(styles, /prefers-reduced-motion: reduce/);
});
