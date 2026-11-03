import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { publicGenerationConfig, resumeGenerationImageEdit } from "./avatar-pipeline/session.js";
import { DEFAULT_VOLCANO_IMAGE_MODEL } from "./avatar-pipeline/providers/volcano-image.js";

const journal = (config) => JSON.parse(JSON.stringify(publicGenerationConfig(config)));

test("journal pins effective image model without credentials or URL overrides", () => {
  const config = journal({ provider: "volcengine", imageModel: "ep-ignored", apiKey: "fixture-video-secret",
    baseUrl: "https://private.example", imageEdit: { model: "ep-original", apiKey: "fixture-image-secret", baseUrl: "https://private-image.example" }, advanced: { token: "fixture-secret" } });
  assert.equal(config.imageModel, "ep-original");
  assert.doesNotMatch(JSON.stringify(config), /secret|private\.example|private-image|apiKey|baseUrl|imageEdit|advanced/);
  const resumed = resumeGenerationImageEdit({ config }, { model: "ep-new-settings", apiKey: "fixture-current-key" });
  assert.equal(resumed.model, "ep-original");
  assert.equal(resumed.apiKey, "fixture-current-key");
  assert.equal(journal({ provider: "kling" }).imageModel, DEFAULT_VOLCANO_IMAGE_MODEL);
  assert.equal(journal({ imageModel: " ep-direct " }).imageModel, "ep-direct");
});

async function resume(session) {
  const calls = [];
  const source = readFileSync(new URL("./generation-task.js", import.meta.url), "utf8");
  const start = source.indexOf("export async function resumeGenerationTask(");
  const end = source.indexOf("export async function dismissGenerationSession(", start);
  assert.ok(start >= 0 && end > start);
  const context = vm.createContext({ File, Uint8Array, resumeGenerationImageEdit,
    isGenerationRunning: () => false, loadGenerationSession: async () => structuredClone(session),
    loadProviderConfig: () => ({ apiKey: "fixture-video-current", imageModel: "ep-new-settings" }),
    loadArkImageConfig: () => ({ apiKey: "fixture-image-current", model: "ep-new-settings" }),
    startGenerationTask: (input) => calls.push(input),
  });
  vm.runInContext(source.slice(start, end).replace(/^export /, "") + "\nthis.resume = resumeGenerationTask;", context);
  await context.resume();
  return calls;
}

test("actual resume entry keeps original image model and cached results after settings change", async () => {
  for (const provider of ["volcengine", "kling"]) {
    const session = { config: journal({ provider, imageEdit: { model: "ep-original" } }),
      reference: { bytes: new Uint8Array([1]) }, actions: { welcome: { startFrameApproved: true, startFrame: { bytes: new Uint8Array([2]) } } } };
    const before = structuredClone(session);
    const calls = await resume(session);
    assert.equal(calls.length, 1);
    assert.equal(calls[0].providerConfig.imageEdit.model, "ep-original");
    assert.equal(calls[0].providerConfig.imageEdit.apiKey, "fixture-image-current");
    assert.equal(calls[0].session.actions.welcome.startFrameApproved, true);
    assert.equal(calls[0].session.actions.welcome.startFrame.bytes[0], 2);
    assert.deepEqual(session, before);
  }
});

test("default model is frozen too when the current default selection changes", async () => {
  const calls = await resume({ config: journal({ provider: "volcengine" }), actions: {} });
  assert.equal(calls[0].providerConfig.imageEdit.model, DEFAULT_VOLCANO_IMAGE_MODEL);
});

test("unidentified legacy image generation stops before task start, preserving paid evidence", async () => {
  const session = { config: { provider: "volcengine" }, imageIntent: true,
    reference: { bytes: new Uint8Array([1]) }, distinctStartFrames: true,
    actions: { welcome: { intent: true, taskId: "accepted-cloud-task" } } };
  const before = structuredClone(session);
  await assert.rejects(resume(session), /旧任务未记录图片模型/);
  assert.deepEqual(session, before);
});

test("legacy conversion-only and explicit original-only jobs remain recoverable", async () => {
  for (const phase of ["converting", "conversion_failed"]) {
    const calls = await resume({ config: { provider: "volcengine" }, phase, appearanceId: "existing", actions: {} });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].session.phase, phase);
  }
  const calls = await resume({ config: { provider: "kling" }, skipProcessing: true, actions: {} });
  assert.equal(calls.length, 1);
  await assert.rejects(resume({ config: { provider: "volcengine" }, phase: "conversion_failed", actions: {} }), /旧任务未记录图片模型/);
  await assert.rejects(resume({ config: { provider: "volcengine" }, skipProcessing: true, distinctStartFrames: true, actions: {} }), /旧任务未记录图片模型/);
});
