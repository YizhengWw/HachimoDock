/**
 * [Input] provider config fragments used by the avatar pipeline runner.
 * [Output] Node regression coverage for choosing a valid thinking-model name.
 * [Pos] test node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { resolveThinkingModelName } from "./run.js";
import { DEFAULT_THINKING_MODEL } from "./thinking-model.js";

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
