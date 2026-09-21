/**
 * [Input] Volcano Ark image-to-video task payload builder.
 * [Output] Regression coverage for Ark payloads, stage-specific image moderation, conservative portrait classification, manual MP4 guidance, model access and non-retryable failures.
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
  getVolcanoInputImageRejection,
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

test("image policy rejection does not retry or poison the model cache", async () => {
  const original = globalThis.fetch;
  let calls = 0;
  const error = { code: "InputImageSensitiveContentDetected.PolicyViolation", message: "input image may relate to copyright restrictions. Request id: test-policy" };
  globalThis.fetch = async () => {
    calls += 1;
    return Response.json({ error }, { status: 400 });
  };
  try {
    const config = { apiKey: "test-policy-key", model: "doubao-seedance-test" };
    for (let i = 0; i < 2; i++) {
      await assert.rejects(runVolcanoFamily({ config, prompt: "move" }), (err) => {
        assert.match(err.message, /参考图未通过版权审核/);
        assert.match(err.message, /Request id: test-policy/);
        assert.equal(getVolcanoInputImageRejection(err)?.code, error.code);
        return true;
      });
      assert.equal(calls, i + 1);
      assert.equal(isVolcanoModelRejected(config.apiKey, config.model), false);
    }
  } finally { globalThis.fetch = original; }
});

test("only explicit image moderation stops a shared image, including bounded error bodies", () => {
  const raw = 'volcano submit HTTP 400: {"error":{"code":"InputImageSensitiveContentDetected.PolicyViolation","message":"copyright restrictions ';
  assert.match(getVolcanoInputImageRejection(raw + "x".repeat(500))?.message, /版权审核/);
  for (const code of ["InputTextSensitiveContentDetected.PolicyViolation", "OutputVideoSensitiveContentDetected", "InvalidParameter", "BadRequest"]) {
    assert.equal(getVolcanoInputImageRejection(JSON.stringify({ error: { code, message: "copyright" } })), null);
  }
  assert.match(getVolcanoInputImageRejection(JSON.stringify({ error: { code: "InputImageSensitiveContentDetected", message: "policy" } }))?.message, /内容审核/);
});

test("moderation identifies the failed stage without treating every policy error as a portrait", () => {
  const rejection = (message, prefix = "volcano submit HTTP 400: ", code = "InputImageSensitiveContentDetected.PolicyViolation") =>
    getVolcanoInputImageRejection(prefix + JSON.stringify({ error: { code, message } }));
  const copyright = rejection("input image may relate to copyright restrictions");
  assert.equal(copyright.category, "copyright");
  assert.match(copyright.message, /^图生视频失败：/);
  assert.match(copyright.message, /不能单独确认是人像问题/);
  assert.match(copyright.message, /暂未接入该素材库/);
  const portrait = rejection("The input image contains a real person.");
  assert.equal(portrait.category, "portrait");
  assert.match(portrait.message, /参考图未通过人像审核/);
  assert.doesNotMatch(portrait.message, /不能单独确认/);
  const privacy = rejection("input image violates policy", undefined, "InputImageSensitiveContentDetected.PrivacyInformation");
  assert.equal(privacy.category, "content");
  const edit = rejection("copyright restrictions", "Ark image edit (doubao-seedream-test) HTTP 400: ");
  assert.match(edit.message, /^图片背景处理（生图）失败：/);
  assert.doesNotMatch(edit.message, /图生视频失败|Seedance/);
});

test("real provider privacy rejection offers the existing manual video upload workflow", () => {
  const raw = 'volcano submit HTTP 400: {"error":{"code":"InputImageSensitiveContentDetected.PrivacyInformation","message":"The request failed because the input image content[1] content[2] may contain real person."}}';
  const rejection = getVolcanoInputImageRejection(raw);
  assert.equal(rejection.category, "portrait");
  assert.match(rejection.message, /可能包含真人或人脸/);
  assert.match(rejection.message, /自行生成动态形象并导出 MP4/);
  assert.match(rejection.message, /形象画廊 → 新建自定义形象 → 自定义上传视频/);
  assert.match(rejection.message, /其他动作.*上传 MP4 替换/);
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
