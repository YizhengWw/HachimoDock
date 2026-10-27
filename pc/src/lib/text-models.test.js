import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { DEFAULT_LITE_MODEL, DOUBAO_PRO_MODEL, normalizeTextModels, latestLiteModel, listThinkingModels, modelsUrl } from "./text-models.js";
import { readJsonOrThrow, shouldRetryTransientHttp, withRetry } from "./avatar-pipeline/http.js";
import { callThinkingModel } from "./avatar-pipeline/thinking-model.js";

test("text catalog filters retired/media models and picks newest Lite numerically, never Pro", () => {
  const models = normalizeTextModels([
    { id: "doubao-seed-2-0-lite-260428" }, { id: DEFAULT_LITE_MODEL }, { id: DOUBAO_PRO_MODEL },
    { id: "doubao-seed-2-1-lite-260628", status: "Retiring" }, { id: "shutdown", status: "Shutdown" },
    { id: "seedance-video" }, { id: "vision", modalities: { input_modalities: ["image"], output_modalities: ["text"] } },
    { id: "text-only", modalities: { input_modalities: ["text"], output_modalities: ["text"] } },
    { id: "off", enabled: false }, { id: DEFAULT_LITE_MODEL },
  ], true);
  assert.equal(models.includes("text-only"), false);
  assert.equal(models.includes("vision"), true);
  assert.equal(models.length, 4);
  assert.equal(latestLiteModel(models), DEFAULT_LITE_MODEL);
  assert.equal(latestLiteModel([DOUBAO_PRO_MODEL]), "");
  assert.equal(latestLiteModel(["doubao-seed-2-9-lite-270101", "doubao-seed-2-10-lite-270101"]), "doubao-seed-2-10-lite-270101");
});

test("vision model discovery is GET only, bounded timeout, no prompt or paid request", async () => {
  let called;
  const models = await listThinkingModels({ apiKey: "test-key", baseUrl: "https://ark.cn-beijing.volces.com" }, async (url, init) => {
    called = { url, init };
    return Response.json({ data: [{ id: DEFAULT_LITE_MODEL }, { id: DOUBAO_PRO_MODEL }] });
  });
  assert.equal(called.url, "https://ark.cn-beijing.volces.com/api/v3/models");
  assert.equal(called.init.method, "GET");
  assert.equal(called.init.timeoutMs, 15000);
  assert.equal(called.init.body, undefined);
  assert.deepEqual(models, [DEFAULT_LITE_MODEL, DOUBAO_PRO_MODEL]);
  assert.equal(modelsUrl("https://ark.cn-beijing.volces.com/api/v3/"), called.url);
  assert.throws(() => modelsUrl("https://key:secret@host.invalid"));
  await assert.rejects(listThinkingModels({ apiKey: "test", baseUrl: "https://host.invalid" }, async () => Response.json({ data: [], has_more: true })), /不完整/);
});

test("404 and other permanent errors are attempted exactly once, transient HTTP errors have bounded retries", async () => {
  for (const status of [400, 401, 403, 404, 422]) {
    let count = 0;
    await assert.rejects(withRetry(async () => {
      count++;
      return readJsonOrThrow(new Response('{"error":"not found"}', { status }), "thinking model");
    }, { retries: 3, baseDelayMs: 1, shouldRetry: shouldRetryTransientHttp }), error => error.status === status);
    assert.equal(count, 1);
  }
  let count = 0;
  await assert.rejects(withRetry(async () => { count++; return readJsonOrThrow(new Response("busy", { status: 503 }), "thinking model"); },
    { retries: 2, baseDelayMs: 1, shouldRetry: shouldRetryTransientHttp }));
  assert.equal(count, 3);
  assert.equal(shouldRetryTransientHttp(new Error("network delivery unknown")), false);
});

test("thinking POST 404 does not retry or silently replace a selected Pro", async () => {
  const originalFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, init) => { calls.push(JSON.parse(init.body)); return new Response('{"error":{"code":"InvalidEndpointOrModel.NotFound"}}', { status: 404 }); };
  try {
    await assert.rejects(callThinkingModel({ thinking: { apiKey: "test", model: DOUBAO_PRO_MODEL }, image: {} }), /重新选择/);
    assert.equal(calls.length, 1);
    assert.equal(calls[0].model, DOUBAO_PRO_MODEL);
  } finally { globalThis.fetch = originalFetch; }
});

test("both generation entry points honor selected thinking model and use the shared dropdown", () => {
  for (const name of ["CustomAvatarWizard", "AppearanceDetail"]) {
    const source = readFileSync(new URL(`../${name}.jsx`, import.meta.url), "utf8");
    assert.match(source, /thinkingModel: thinkingModel\.trim\(\) \|\|/);
    assert.match(source, /onThinkingModel=\{setThinkingModel\}/);
  }
  const selector = readFileSync(new URL("../TextModelSelect.jsx", import.meta.url), "utf8");
  assert.match(selector, /list_voice_chat_models/);
  assert.match(selector, /手动填写模型/);
  assert.match(selector, /cancelled = true/);
});
