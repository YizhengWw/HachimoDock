/**
 * [Input] provider config fragments used by the avatar pipeline runner.
 * [Output] Node regression coverage for choosing a valid thinking-model name, custom-generation family filtering,
 *          and building single-state retry manifests.
 * [Pos] test node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { buildSingleFamilyManifest, resolveThinkingModelName } from "./run.js";
import { DEFAULT_THINKING_MODEL } from "./thinking-model.js";
import * as familyModule from "./families.js";

const srcDir = dirname(fileURLToPath(import.meta.url));

function readSource(fileName) {
  return readFileSync(join(srcDir, fileName), "utf8");
}

test("Volcano video model names are never reused as the thinking model", () => {
  assert.equal(
    resolveThinkingModelName({
      provider: "volcengine",
      model: "doubao-seedance-2-0-260128",
    }),
    DEFAULT_THINKING_MODEL,
  );

  assert.equal(
    resolveThinkingModelName({
      provider: "volcengine",
      model: "doubao-seedance-2-0-260128",
      thinkingModel: "doubao-seedance-2-0-260128",
    }),
    DEFAULT_THINKING_MODEL,
  );
});

test("explicit non-video thinking models are preserved", () => {
  assert.equal(
    resolveThinkingModelName({
      provider: "volcengine",
      model: "doubao-seedance-2-0-260128",
      thinkingModel: "doubao-seed-2-0-pro-260215",
    }),
    "doubao-seed-2-0-pro-260215",
  );

  assert.equal(
    resolveThinkingModelName({
      provider: "custom",
      model: "custom-video-model",
      thinkingModel: "custom-thinking-model",
    }),
    "custom-thinking-model",
  );
});

test("single-family retry manifest uses only the selected state and user prompt", () => {
  const manifest = buildSingleFamilyManifest({
    family: {
      family: "working",
      label: "working",
      playback: "loop_state",
      motion_brief: "desk work",
    },
    prompt: "  cute cat writes notes on a desk  ",
  });

  assert.equal(manifest.mode, "single_family_video");
  assert.equal(manifest.entries.length, 1);
  assert.equal(manifest.entries[0].family, "working");
  assert.equal(manifest.entries[0].prompt, "cute cat writes notes on a desk");
});

test("custom avatar generation skips importer-only directional touch states", () => {
  const runSource = readSource("run.js");
  const taskSource = readFileSync(join(srcDir, "../generation-task.js"), "utf8");
  const allIds = familyModule.FAMILIES.map((item) => item.family);
  const customFamilies = familyModule.CUSTOM_GENERATION_FAMILIES;

  assert.ok(allIds.includes("touch.right"));
  assert.ok(allIds.includes("touch.left"));
  assert.ok(Array.isArray(customFamilies));

  const customIds = customFamilies.map((item) => item.family);
  assert.doesNotMatch(customIds.join("\n"), /^touch\.right$/m);
  assert.doesNotMatch(customIds.join("\n"), /^touch\.left$/m);
  assert.ok(customIds.includes("touch.lick"));
  assert.ok(customIds.includes("touch.what"));
  assert.match(runSource, /CUSTOM_GENERATION_FAMILIES/);
  assert.match(taskSource, /CUSTOM_GENERATION_FAMILIES/);
});
