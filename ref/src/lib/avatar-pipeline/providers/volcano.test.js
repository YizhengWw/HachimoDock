/**
 * [Input] Volcano Ark image-to-video task payload builder.
 * [Output] Node regression coverage for Ark v3 first/last-frame task payload shape, diagnostics, and account-actionable errors.
 * [Pos] test node in ref/src/lib/avatar-pipeline/providers
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  buildVolcanoTaskDiagnostics,
  buildVolcanoTaskPayload,
  normalizeVolcanoSubmitErrorMessage,
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
      text: "无人机以极快速度穿越复杂障碍或自然奇观 --duration 5 --camerafixed false --watermark false",
    },
    { type: "image_url", image_url: { url: "data:image/png;base64,abc123" }, role: "first_frame" },
    { type: "image_url", image_url: { url: "data:image/png;base64,abc123" }, role: "last_frame" },
  ]);
  assert.equal("duration" in payload, false);
  assert.equal("ratio" in payload, false);
  assert.equal("resolution" in payload, false);
  assert.equal("generate_audio" in payload, false);
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

  assert.match(message, /火山引擎模型未开通/);
  assert.match(message, /doubao-seedance-2-0-260128/);
  assert.match(message, /doubao-seedance-1-5-pro-251215/);
  assert.match(message, /Ark 控制台/);
  assert.match(message, /原始错误/);
});
