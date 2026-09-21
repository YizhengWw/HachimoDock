/**
 * [Input] Raw provider and per-family generation errors.
 * [Output] Regression coverage for actionable failures, reference-image policy rejection, manual MP4 guidance and skipped-action summaries.
 * [Pos] test node in pc/src/lib
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import {
  buildAllFamiliesFailedMessage,
  normalizeGenerationErrorMessage,
} from "./generation-task.js";

test("terminal acknowledgement ignores older toasts and running tasks", () => {
  // Exercise the real state machine in isolation; no provider calls or user data.
  const source = readFileSync(new URL("./generation-task.js", import.meta.url), "utf8")
    .replace(/^import .*;\n/gm, "")
    .replace(/^export /gm, "");
  const context = vm.createContext({});
  vm.runInContext(source + "\nthis.api = { setState, getGenerationTask, acknowledgeGenerationTask };", context);
  const { setState, getGenerationTask, acknowledgeGenerationTask } = context.api;
  setState({ status: "failed", completionEpoch: 2, error: "test" });
  acknowledgeGenerationTask(1);
  assert.equal(getGenerationTask().status, "failed");
  acknowledgeGenerationTask(2);
  assert.equal(getGenerationTask().status, "idle");
  setState({ status: "running", completionEpoch: 2 });
  acknowledgeGenerationTask(2);
  assert.equal(getGenerationTask().status, "running");
  setState({ status: "completed", completionEpoch: 3 });
  acknowledgeGenerationTask(2);
  assert.equal(getGenerationTask().status, "completed");
  acknowledgeGenerationTask(3);
  assert.equal(getGenerationTask().status, "idle");
});

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

test("portrait upload alternative survives the all-actions-failed summary", () => {
  const raw = 'volcano submit HTTP 400: {"error":{"code":"InputImageSensitiveContentDetected.PrivacyInformation","message":"input image may contain real person"}}';
  const summary = normalizeGenerationErrorMessage(buildAllFamiliesFailedMessage([
    { family: "welcome", ok: false, error: raw },
    { family: "working", ok: false, skipped: true, error: raw },
  ]));
  assert.match(summary, /参考图未通过人像审核/);
  assert.match(summary, /自行生成动态形象并导出 MP4/);
  assert.match(summary, /形象画廊 → 新建自定义形象 → 自定义上传视频/);
  assert.match(summary, /上传 MP4 替换/);
  assert.equal((summary.match(/自行生成动态形象/g) || []).length, 1);
});

test("image copyright rejection is not misreported as bad parameters or credentials", () => {
  const raw = 'volcano submit HTTP 400: {"error":{"code":"InputImageSensitiveContentDetected.PolicyViolation","message":"The input image content[1] content[2] may be related to copyright restrictions. Request id: test-id","type":"BadRequest"}}';
  const message = normalizeGenerationErrorMessage(raw);
  assert.match(message, /^图生视频失败：/);
  assert.match(message, /参考图未通过版权审核/);
  assert.match(message, /可能涉及版权限制/);
  assert.match(message, /复核/);
  assert.doesNotMatch(message, /请求参数|API Key|模型不可用/);
  const summary = buildAllFamiliesFailedMessage([
    { family: "welcome", ok: false, error: raw },
    { family: "working", ok: false, skipped: true, error: raw },
  ]);
  assert.equal((summary.match(/参考图未通过版权审核/g) || []).length, 1);
  assert.match(summary, /停止提交剩余 1 个动作/);
  assert.match(summary, /已完成的结果保留/);
  assert.match(normalizeGenerationErrorMessage('volcano submit HTTP 400: {"error":{"code":"InvalidParameter","message":"unsupported ratio"}}'), /请求参数不被当前模型接受/);
});
