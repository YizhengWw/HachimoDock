/**
 * [Input] provider config fragments used by the avatar pipeline runner.
 * [Output] Node regression coverage for choosing a valid thinking-model name, custom-generation family filtering,
 *          single-state retries, shared-image submission stopping, partial success and cancellation.
 * [Pos] test node in pc/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import {
  buildSingleFamilyManifest,
  resolveArkBackgroundConfig,
  resolveThinkingModelName,
  runAvatarPipeline,
} from "./run.js";
import { DEFAULT_THINKING_MODEL } from "./thinking-model.js";
import { DEFAULT_VOLCANO_IMAGE_MODEL } from "./providers/volcano-image.js";
import * as familyModule from "./families.js";

const srcDir = dirname(fileURLToPath(import.meta.url));

async function simulateBatch({ code = "InputImageSensitiveContentDetected.PolicyViolation", pollFailure = false, successes = false, abort = false } = {}) {
  const originalFetch = globalThis.fetch;
  const originalLog = console.error;
  const controller = new AbortController();
  let submissions = 0;
  const saved = [];
  const progress = [];
  const failure = { code, message: "input may relate to copyright restrictions. Request id: simulated" };
  globalThis.fetch = async (url, init) => {
    if (String(url).endsWith("/responses")) return Response.json({ output_text: JSON.stringify({
      persona: {}, prompts: familyModule.CUSTOM_GENERATION_FAMILIES.map(({ family }) => ({ family, prompt: "move" })),
    }) });
    if (init.method === "POST") {
      submissions++;
      if (abort) controller.abort();
      if (pollFailure || (successes && submissions > 1)) return Response.json({ id: `task-${submissions}` });
      return Response.json({ error: failure }, { status: 400 });
    }
    if (String(url).includes("/tasks/")) {
      // Let the first submission rejection reach the scheduler before in-flight successes finish.
      await new Promise((resolve) => setTimeout(resolve, 5));
      if (pollFailure) return Response.json({ status: "failed", padding: "x".repeat(600), error: failure });
      return Response.json({ status: "succeeded", content: { video_url: "https://video.example.test/result.mp4" } });
    }
    if (String(url) === "https://video.example.test/result.mp4") return new Response(new Uint8Array([1, 2, 3]));
    throw new Error(`Unexpected test request: ${url}`);
  };
  console.error = () => {};
  try {
    const result = await runAvatarPipeline({
      imageFile: new File([new Uint8Array([1, 2])], "test.png", { type: "image/png" }),
      providerConfig: { provider: "volcengine", apiKey: "test-batch-key", model: "doubao-seedance-2-0-fast-test" },
      skipProcessing: true,
      signal: controller.signal,
      onProgress: (value) => progress.push(value),
      onFamilyDone: async ({ family }) => saved.push(family),
    });
    return { result, submissions, saved, progress };
  } finally { globalThis.fetch = originalFetch; console.error = originalLog; }
}

test("shared reference rejection stops unsubmitted actions and completes progress", async () => {
  for (const pollFailure of [false, true]) {
    const { result, submissions, progress } = await simulateBatch({ pollFailure });
    assert.equal(submissions, 3);
    assert.equal(result.families.length, familyModule.CUSTOM_GENERATION_FAMILIES.length);
    assert.equal(result.families.filter((f) => f.skipped).length, result.families.length - 3);
    assert.ok(result.families.every((f) => !f.ok && /版权审核/.test(f.error)));
    assert.equal(progress.at(-1).completed, progress.at(-1).total);
    assert.match(progress.at(-1).message, /参考图未通过版权审核/);
    assert.match(progress.at(-1).message, /成功生成 0 个动作/);
  }
});

test("already submitted successes are saved and a new batch is not blocked", async () => {
  for (let attempt = 0; attempt < 2; attempt++) {
    const { result, submissions, saved, progress } = await simulateBatch({ successes: true });
    assert.equal(submissions, 3);
    assert.equal(saved.length, 2);
    assert.match(progress.at(-1).message, /成功生成 2 个动作/);
    assert.match(progress.at(-1).referenceImageError, /版权审核/);
    assert.equal(result.families.filter((f) => f.ok).length, 2);
    assert.deepEqual(saved.map((f) => f.videoBytes), [new Uint8Array([1, 2, 3]), new Uint8Array([1, 2, 3])]);
  }
});

test("per-action text/output and parameter failures do not stop unrelated actions", async () => {
  for (const code of ["InputTextSensitiveContentDetected.PolicyViolation", "OutputVideoSensitiveContentDetected", "InvalidParameter"]) {
    const { result, submissions } = await simulateBatch({ code });
    assert.equal(submissions, familyModule.CUSTOM_GENERATION_FAMILIES.length);
    assert.ok(result.families.every((f) => !f.skipped));
  }
});

test("user cancellation still propagates", async () => {
  await assert.rejects(simulateBatch({ abort: true }), { name: "AbortError" });
});

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

test("Ark background editing reuses the independent Volcengine credential", () => {
  assert.deepEqual(
    resolveArkBackgroundConfig({
      provider: "volcengine",
      apiKey: "ark-key",
      baseUrl: "https://ark.example.test",
    }),
    {
      apiKey: "ark-key",
      baseUrl: "https://ark.example.test",
      model: DEFAULT_VOLCANO_IMAGE_MODEL,
    },
  );
  assert.deepEqual(
    resolveArkBackgroundConfig({
      provider: "kling",
      apiKey: "kling-key",
      baseUrl: "https://kling.example.test",
      imageEdit: {
        apiKey: "ark-key",
        baseUrl: "https://ark.example.test",
        model: "seedream-test",
      },
    }),
    {
      apiKey: "ark-key",
      baseUrl: "https://ark.example.test",
      model: "seedream-test",
    },
  );
  assert.equal(resolveArkBackgroundConfig({ provider: "kling", apiKey: "kling-key" }), null);
  assert.equal(resolveArkBackgroundConfig({ provider: "volcengine", apiKey: "" }), null);
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

test("single-family retry forwards image-processing progress into UI progress events", () => {
  const runSource = readSource("run.js");

  assert.match(runSource, /onProgress:\s*\(stage,\s*progress\) =>/);
  assert.match(runSource, /emit\(\{\s*stage:\s*"processing",\s*message:[\s\S]*progress\s*\}\)/);
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
