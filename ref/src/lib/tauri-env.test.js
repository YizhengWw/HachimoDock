/**
 * [Input] `hasTauriRuntime` from `./tauri-env.js`.
 * [Output] Behavior coverage (bare node) that the runtime probe is false without
 *          a Tauri window and true only when window.__TAURI_INTERNALS__ is set.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { hasTauriRuntime } from "./tauri-env.js";

test("hasTauriRuntime is false without a window", () => {
  assert.equal(typeof globalThis.window, "undefined");
  assert.equal(hasTauriRuntime(), false);
});

test("hasTauriRuntime reflects window.__TAURI_INTERNALS__ presence", () => {
  globalThis.window = {};
  try {
    assert.equal(hasTauriRuntime(), false);
    globalThis.window.__TAURI_INTERNALS__ = { invoke() {} };
    assert.equal(hasTauriRuntime(), true);
  } finally {
    delete globalThis.window;
  }
});
