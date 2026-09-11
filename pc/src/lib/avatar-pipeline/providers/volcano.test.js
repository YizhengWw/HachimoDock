/**
 * [Input] Volcano Ark image-to-video task payload builder.
 * [Output] Node regression coverage for Ark v3 first/last-frame task payload shape, diagnostics, and account-actionable errors.
 * [Pos] test node in pc/src/lib/avatar-pipeline/providers
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  buildVolcanoTaskDiagnostics,
  buildVolcanoTaskPayload,
  normalizeVolcanoSubmitErrorMessage,
  runVolcanoFamily,
  isVolcanoModelRejected,
  clearVolcanoModelRejections,
} from "./volcano.js";

test("Seedance 2.0 payload includes matching first and last frame references", () => {
  const payload = buildVolcanoTaskPayload({
    model: "doubao-seedance-2-0-260128",
    prompt: "第一人称果茶广告",
    imageDataUrl: "data:image/png;base64,abc123",
    duration: 11,
    ratio: "16:9",
    generateAudio: true,
    watermark: false,
  });

  assert.equal(payload.model, "doubao-seedance-2-0-260128");
  assert.deepEqual(payload.content, [
    { type: "text", text: "第一人称果茶广告" },
    { type: "image_url", image_url: { url: "data:image/png;base64,abc123" }, role: "first_frame" },
    { type: "image_url", image_url: { url: "data:image/png;base64,abc123" }, role: "last_frame" },
  ]);
  assert.equal(payload.generate_audio, true);
  assert.equal(payload.ratio, "16:9");
  assert.equal(payload.duration, 11);
  assert.equal(payload.watermark, false);
  assert.equal(payload.content[1].role, "first_frame");
  assert.equal(payload.content[2].role, "last_frame");
});

test("Seedance 1.5 payload includes prompt flags plus matching first and last frame references", () => {
  const payload = buildVolcanoTaskPayload({
    model: "doubao-seedance-1-5-pro-251215",
    prompt: "无人机以极快速度穿越复杂障碍或自然奇观",
    imageDataUrl: "data:image/png;base64,abc123",
    duration: 5,
    ratio: "4:3",
    resolution: "480p",
    generateAudio: false,
    watermark: false,
  });

  assert.equal(payload.model, "doubao-seedance-1-5-pro-251215");
  assert.deepEqual(payload.content, [
    {
      type: "text",
      text: "无人机以极快速度穿越复杂障碍或自然奇观 --duration 5 --camerafixed false --watermark false --ratio 4:3 --resolution 480p",
    },
    { type: "image_url", image_url: { url: "data:image/png;base64,abc123" }, role: "first_frame" },
    { type: "image_url", image_url: { url: "data:image/png;base64,abc123" }, role: "last_frame" },
  ]);
  assert.equal("duration" in payload, false);
  assert.equal("ratio" in payload, false);
  assert.equal("resolution" in payload, false);
  assert.equal(payload.generate_audio, false);
  assert.equal("watermark" in payload, false);
});

test("Volcano submit diagnostics omit secrets and raw media", () => {
  const payload = buildVolcanoTaskPayload({
    model: "doubao-seedance-2-0-260128",
    prompt: "move",
    imageDataUrl: "data:image/png;base64,abc123",
    duration: 5,
    ratio: "4:3",
    resolution: "480p",
  });

  const diagnostics = buildVolcanoTaskDiagnostics(payload);
  assert.deepEqual(diagnostics.content.map((part) => part.role || part.type), ["text", "first_frame", "last_frame"]);
  assert.equal(diagnostics.content[1].url.kind, "data_url");
  assert.equal(diagnostics.content[1].url.mime, "image/png");
  assert.equal(JSON.stringify(diagnostics).includes("abc123"), false);
});

test("Volcano ModelNotOpen submit error becomes an actionable account message", () => {
  const message = normalizeVolcanoSubmitErrorMessage(
    'volcano submit HTTP 404: {"error":{"code":"ModelNotOpen","message":"Your account has not activated the model doubao-seedance-2-0-260128. Please activate the model service in the Ark Console. Request id: abc","type":"Not Found"}}',
    "doubao-seedance-2-0-260128",
  );

  assert.match(message, /火山引擎模型不可用/);
  assert.match(message, /doubao-seedance-2-0-260128/);
  assert.doesNotMatch(message, /doubao-seedance-1-5-pro-251215/);
  assert.match(message, /Ark 控制台/);
  assert.match(message, /刷新视频模型列表/);
});

test("explicit video controls reach the Ark request", () => {
  const payload = buildVolcanoTaskPayload({ model: "doubao-seedance-2-0-260128", prompt: "move",
    duration: 10, resolution: "720p", seed: 42, cameraFixed: true, generateAudio: true, watermark: true });
  assert.equal(payload.duration, 10);
  assert.equal(payload.resolution, "720p");
  assert.equal(payload.seed, 42);
  assert.equal(payload.camera_fixed, true);
  assert.equal(payload.generate_audio, true);
  assert.equal(payload.watermark, true);
  assert.throws(() => buildVolcanoTaskPayload({ seed: 0.5 }), /整数/);
  const defaults = buildVolcanoTaskPayload({ duration: "auto", resolution: "auto" });
  assert.equal(defaults.duration, undefined);
  assert.equal(defaults.resolution, undefined);
});

test("unavailable model is not retried and is excluded for this API Key", async () => {
  const original = globalThis.fetch;
  let calls = 0;
  globalThis.fetch = async () => {
    calls += 1;
    return Response.json({ error: { code: "InvalidEndpointOrModel.NotFound", message: "model unavailable" } }, { status: 404 });
  };
  try {
    const config = { apiKey: "test-rejected-key", model: "doubao-seedance-unavailable" };
    await assert.rejects(runVolcanoFamily({ config, prompt: "move" }), /模型不可用/);
    assert.equal(calls, 1);
    assert.equal(isVolcanoModelRejected(config.apiKey, config.model), true);
    await assert.rejects(runVolcanoFamily({ config, prompt: "move" }), /无法调用/);
    assert.equal(calls, 1);
    assert.equal(isVolcanoModelRejected("another-test-key", config.model), false);
    assert.equal(isVolcanoModelRejected(config.apiKey, config.model, Date.now() + 6 * 60_000), false);
    await assert.rejects(runVolcanoFamily({ config, prompt: "move" }), /模型不可用/);
    assert.equal(isVolcanoModelRejected(config.apiKey, config.model), true);
    clearVolcanoModelRejections(config.apiKey);
    assert.equal(isVolcanoModelRejected(config.apiKey, config.model), false);
  } finally { globalThis.fetch = original; }
});
