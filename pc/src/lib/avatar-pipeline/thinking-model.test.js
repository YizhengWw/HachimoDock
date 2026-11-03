/**
 * [Input] Volcano Ark Responses API thinking-model request builder.
 * [Output] Node regression coverage for requested-family prompts, multimodal/text-only payloads, and model-specific access failures.
 * [Pos] test node in pc/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  DEFAULT_THINKING_MODEL,
  buildThinkingModelRequest,
  callThinkingModel,
} from "./thinking-model.js";

test("analysis access failures retain the actual requested model ID", async () => {
  const originalFetch = globalThis.fetch;
  let calls = 0;
  globalThis.fetch = async () => {
    calls++;
    return Response.json({ error: { code: "ModelNotOpen", message: "model is not activated" } }, { status: 404 });
  };
  try {
    await assert.rejects(callThinkingModel({ thinking: { apiKey: "fixture-only", model: "ep-analysis-fixture" }, image: {} }), error => {
      assert.match(error.message, /thinking model \(ep-analysis-fixture\) HTTP 404/);
      assert.equal(error.status, 404);
      return true;
    });
    assert.equal(calls, 1);
  } finally { globalThis.fetch = originalFetch; }
});

test('prompt JSON is constrained, persisted before parse and replayed locally without billing',async()=>{
  assert.equal(buildThinkingModelRequest({thinking:{},image:{}}).body.text.format.type,'json_object');
  const old=globalThis.fetch;let calls=0,saved;
  globalThis.fetch=async()=>{calls++;return Response.json({output_text:'{persona: {}}'});};
  try {
    const args={thinking:{apiKey:'fixture'},image:{},onResponse:async value=>{saved=value;}};
    await assert.rejects(callThinkingModel(args),e=>e.code==='PROMPT_OUTPUT_INVALID');assert.equal(calls,1);assert.equal(saved.output_text,'{persona: {}}');
    await assert.rejects(callThinkingModel({...args,cachedResponse:saved}));assert.equal(calls,1);
    const valid=await callThinkingModel({...args,cachedResponse:{output_text:'{"persona":{},"prompts":[]}'}});assert.deepEqual(valid.prompts,[]);assert.equal(calls,1);
  }finally{globalThis.fetch=old;}
});

test("default Doubao 2.0 thinking request uses Ark Responses API with image input", () => {
  const request = buildThinkingModelRequest({
    thinking: { apiKey: "test-key" },
    image: { dataUrl: "data:image/png;base64,abc123" },
    appearanceName: "小狗",
    personality: "活泼",
  });

  assert.equal(request.apiUrl, "https://ark.cn-beijing.volces.com/api/v3/responses");
  assert.equal(DEFAULT_THINKING_MODEL, "doubao-seed-2-1-lite-260915");
  assert.equal(request.body.model, DEFAULT_THINKING_MODEL);
  assert.deepEqual(request.body.thinking, {type:"disabled"});
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
  assert.equal(request.body.thinking, undefined);
});

test("explicit reasoning is retained and unknown endpoints receive no guessed thinking switch", () => {
  const base = {image:{dataUrl:"data:image/png;base64,abc"}};
  assert.deepEqual(buildThinkingModelRequest({...base,thinking:{reasoningEffort:"high"}}).body.thinking,{type:"enabled"});
  assert.equal(buildThinkingModelRequest({...base,thinking:{model:"ep-private-endpoint"}}).body.thinking,undefined);
  assert.equal(buildThinkingModelRequest({...base,thinking:{baseUrl:"https://custom.example.test"}}).body.thinking,undefined);
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
