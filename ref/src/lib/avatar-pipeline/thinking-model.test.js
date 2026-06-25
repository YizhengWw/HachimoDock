/**
 * [Input] Volcano Ark Responses API thinking-model request builder.
 * [Output] Node regression coverage for requested-family prompts, `/api/v3/responses` Doubao multimodal, and text-only payloads.
 * [Pos] test node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  DEFAULT_THINKING_MODEL,
  buildThinkingModelRequest,
} from "./thinking-model.js";

test("default Doubao 2.0 thinking request uses Ark Responses API with image input", () => {
  const request = buildThinkingModelRequest({
    thinking: { apiKey: "test-key" },
    image: { dataUrl: "data:image/png;base64,abc123" },
    appearanceName: "小狗",
    personality: "活泼",
  });

  assert.equal(request.apiUrl, "https://ark.cn-beijing.volces.com/api/v3/responses");
  assert.equal(DEFAULT_THINKING_MODEL, "doubao-seed-2-0-pro-260215");
  assert.equal(request.body.model, DEFAULT_THINKING_MODEL);
  assert.equal(request.body.input.length, 1);
  assert.equal(request.body.input[0].role, "user");
  assert.deepEqual(
    request.body.input[0].content.map((part) => part.type),
    ["input_image", "input_text"],
  );
  assert.equal(request.body.input[0].content[0].image_url, "data:image/png;base64,abc123");
  assert.match(request.body.input[0].content[1].text, /小狗/);
  assert.doesNotMatch(request.body.input[0].content[1].text, /纯文本模型，不支持图片输入/);
  assert.equal("messages" in request.body, false);
});

test("text-only DeepSeek thinking models omit input_image", () => {
  const request = buildThinkingModelRequest({
    thinking: { apiKey: "test-key", model: "deepseek-v3-2-251201" },
    image: { dataUrl: "data:image/png;base64,abc123" },
  });

  assert.deepEqual(
    request.body.input[0].content.map((part) => part.type),
    ["input_text"],
  );
  assert.equal(
    request.body.input[0].content.some((part) => "image_url" in part),
    false,
  );
  assert.match(request.body.input[0].content[0].text, /纯文本模型，不支持图片输入/);
});

test("thinking request prompt uses the caller-provided family list", () => {
  const request = buildThinkingModelRequest({
    thinking: { apiKey: "test-key" },
    image: { dataUrl: "data:image/png;base64,abc123" },
    families: [
      {
        family: "working",
        label: "working",
        playback: "loop_state",
        motion_brief: "desk work",
        prop_policy: "Required small marker prop: one tiny keyboard.",
      },
    ],
  });

  const text = request.body.input[0].content.find((part) => part.type === "input_text").text;
  assert.match(text, /"family": "working"/);
  assert.doesNotMatch(text, /touch\.right/);
  assert.doesNotMatch(text, /touch\.left/);
});
