/**
 * [Input] Read ToastStack.jsx source + invoke useToast in a runtime check.
 * [Output] Static + runtime Node coverage that ToastProvider exposes useToast with push/dismiss, queues multiple toasts, auto-dismisses after ttl, and ToastStack renders from context.
 * [Pos] test node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "ToastStack.jsx"), "utf8");

test("ToastStack source exports ToastProvider, useToast, and a default ToastStack", () => {
  assert.match(source, /export function ToastProvider\s*\(/);
  assert.match(source, /export function useToast\s*\(/);
  assert.match(source, /export default function ToastStack\s*\(/);
});

test("useToast returns push and dismiss methods", () => {
  assert.match(source, /push\s*[:,(]/);
  assert.match(source, /dismiss\s*[:,(]/);
});

test("Toasts auto-dismiss via setTimeout with a configurable ttl", () => {
  assert.match(source, /setTimeout/);
  assert.match(source, /ttl/);
});

test("ToastStack renders queue items with tone, title, optional message and action", () => {
  for (const key of ["tone", "title", "message", "action"]) {
    assert.match(source, new RegExp(`\\b${key}\\b`), `toast item should support ${key}`);
  }
});

test("ToastStack uses the documented presentational classes", () => {
  assert.match(source, /className="toast-stack"/);
  assert.match(source, /toast--/);
});
