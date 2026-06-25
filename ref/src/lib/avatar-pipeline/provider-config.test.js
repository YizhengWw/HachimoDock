/**
 * [Input] Avatar provider-config persistence helper source.
 * [Output] Node regression coverage that the wizard and single-state retry UI share one localStorage contract.
 * [Pos] test node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const libDir = dirname(fileURLToPath(import.meta.url));

test("provider config helper owns the shared localStorage key and Volcengine defaults", () => {
  const source = readFileSync(join(libDir, "provider-config.js"), "utf8");

  assert.match(source, /export const STORAGE_KEY_PREFIX = "claw-pet\.video-gen-config\."/);
  assert.match(source, /export const DEFAULT_PROVIDER_ID = "volcengine"/);
  assert.match(source, /export function loadProviderConfig/);
  assert.match(source, /export function saveProviderConfig/);
  assert.match(source, /DEFAULT_VOLCANO_BASE_URL/);
  assert.match(source, /DEFAULT_THINKING_MODEL/);
  assert.match(source, /fastGeneration: saved\.fastGeneration !== false/);
});
