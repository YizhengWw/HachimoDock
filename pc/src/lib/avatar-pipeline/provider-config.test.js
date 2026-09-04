/**
 * [Input] Avatar provider-config persistence helper source, Ark image-model default, and API-key precedence resolver.
 * [Output] Node regression coverage for the shared localStorage contract, Seedream default, and user-config-first internal fallback.
 * [Pos] test node in pc/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { loadArkImageConfig, resolveProviderApiKey } from "./provider-config.js";

const libDir = dirname(fileURLToPath(import.meta.url));

test("provider config helper owns the shared localStorage key and Volcengine defaults", () => {
  const source = readFileSync(join(libDir, "provider-config.js"), "utf8");

  assert.match(source, /export const STORAGE_KEY_PREFIX = "claw-pet\.video-gen-config\."/);
  assert.match(source, /export const DEFAULT_PROVIDER_ID = "volcengine"/);
  assert.match(source, /export function loadProviderConfig/);
  assert.match(source, /export function saveProviderConfig/);
  assert.match(source, /DEFAULT_VOLCANO_BASE_URL/);
  assert.match(source, /DEFAULT_THINKING_MODEL/);
  assert.match(source, /DEFAULT_VOLCANO_IMAGE_MODEL/);
  assert.match(source, /VOLCENGINE_IMAGE_MODEL/);
  assert.match(source, /fastGeneration: saved\.fastGeneration !== false/);
  assert.match(source, /__PET_MANAGER_INTERNAL_CONTENT_API_KEY__/);
});

test("provider API key keeps an explicit user value ahead of the internal fallback", () => {
  assert.equal(resolveProviderApiKey("volcengine", "user-key", "internal-key"), "user-key");
  assert.equal(resolveProviderApiKey("volcengine", "", "internal-key"), "internal-key");
  assert.equal(resolveProviderApiKey("custom", "", "internal-key"), "");
});

test("Ark image editing resolves independently from the selected video provider", () => {
  const storage = {
    getItem(key) {
      return key.endsWith("volcengine")
        ? JSON.stringify({ apiKey: "ark-image-key", fastGeneration: true })
        : null;
    },
  };

  assert.deepEqual(loadArkImageConfig(storage), {
    apiKey: "ark-image-key",
    baseUrl: "https://ark.cn-beijing.volces.com",
    model: "doubao-seedream-5-0-lite-260128",
  });
});
