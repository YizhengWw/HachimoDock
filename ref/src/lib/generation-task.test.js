/**
 * [Input] Raw provider and per-family generation errors.
 * [Output] Node regression coverage for concise, actionable avatar-generation failure messages.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  buildAllFamiliesFailedMessage,
  normalizeGenerationErrorMessage,
} from "./generation-task.js";

test("all-family failures collapse repeated ModelNotOpen errors into one actionable cause", () => {
  const families = ["welcome", "idle.playing", "idle.wandering"].map((family, index) => ({
    family,
    ok: false,
    error: [
      "火山引擎模型未开通：当前 Ark 账号还没有开通 doubao-seedance-2-0-260128。",
      "请在 Ark 控制台开通该模型服务，或先在模型下拉中改用 doubao-seedance-1-5-pro-251215。",
      `原始错误：volcano submit HTTP 404: {"error":{"code":"ModelNotOpen","message":"Request id: req-${index}"}}`,
      'Volcano payload summary: {"model":"doubao-seedance-2-0-260128"}',
    ].join("\n"),
  }));

  const message = buildAllFamiliesFailedMessage(families);

  assert.match(message, /生成失败：所有动作都没有生成成功/);
  assert.equal((message.match(/火山引擎模型未开通/g) || []).length, 1);
  assert.match(message, /影响动作：welcome、idle\.playing、idle\.wandering/);
  assert.match(message, /诊断摘要/);
  assert.doesNotMatch(message, /idle\.playing: volcano submit HTTP 404/);
  assert.equal(normalizeGenerationErrorMessage(message), message);
});

test("raw provider errors are translated into product-level guidance", () => {
  assert.match(
    normalizeGenerationErrorMessage(
      'volcano submit HTTP 400: {"error":{"code":"InvalidParameter","message":"expected the height to be at least 300px, but received a 320x240px image instead"}}',
    ),
    /参考图尺寸不满足火山引擎要求/,
  );

  assert.match(
    normalizeGenerationErrorMessage(
      'thinking model HTTP 400: {"error":{"code":"InvalidParameter","message":"Model do not support image input","param":"image_url"}}',
    ),
    /文字分析模型不支持图片输入/,
  );

  assert.match(
    normalizeGenerationErrorMessage('volcano submit HTTP 401: {"error":{"message":"invalid api key"}}'),
    /API Key 无效或已过期/,
  );
});
