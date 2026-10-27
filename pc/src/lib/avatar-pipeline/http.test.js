import test from "node:test";
import assert from "node:assert/strict";
import { pipelineFetch, readJsonOrThrow } from "./http.js";

test('already-cancelled operation cannot initiate another paid HTTP request',async()=>{
  const old=globalThis.fetch;let count=0;globalThis.fetch=async()=>{count++;return Response.json({});};
  try{await assert.rejects(pipelineFetch('https://fixture.invalid',{method:'POST',signal:AbortSignal.abort()}),e=>e.name==='AbortError');assert.equal(count,0);}
  finally{globalThis.fetch=old;}
});
import { getGenerationDiagnostics, parseNativeDiagnostic, recordDiagnostic, formatDiagnostic } from "./diagnostics.js";
import { callThinkingModel } from "./thinking-model.js";
import { runVolcanoFamily } from "./providers/volcano.js";
import { editImageBackgroundWithArk } from "./providers/volcano-image.js";
import { processImageForPipeline } from "./image-processing.js";
import { normalizeGenerationErrorMessage } from "../generation-task.js";

test("output image moderation stops preprocessing once even with legacy original fallback enabled", async () => {
  let calls = 0;
  await withInvoke(async (_command, args) => {
    calls++;
    assert.equal(args.diagnosticStage, "image_edit");
    return { status:400, body:JSON.stringify({error:{code:"OutputImageSensitiveContentDetected.PolicyViolation",message:"output image may be related to copyright restrictions. Request id: output-test-123"}}), diagnostic:{...fixture("image_edit"),outcome:"http_error",status:400} };
  }, async () => {
    await assert.rejects(processImageForPipeline(new Blob(["fixture"],{type:"image/png"}), {
      cloudBackground:{apiKey:"fixture-key"}, allowOriginalFallback:true,
      onProgress:stage=>assert.equal(stage,"cloud_editing"),
    }), error=>{
      const message=normalizeGenerationErrorMessage(error.message);
      assert.match(message,/处理后的生成结果.*可能涉及版权限制/);
      assert.match(message,/尚未提交视频任务/);
      assert.match(message,/output-test-123/);
      assert.doesNotMatch(message,/请求参数不被|可重试处理|关闭背景处理/);
      return true;
    });
  });
  assert.equal(calls,1);
});

const fixture = (stage = "thinking") => ({
  localId: "local-fixture", timestampMs: Date.now(), stage, phase: "response_body",
  elapsedMs: 120003, timeoutMs: 120000, requestId: "req-fixture-123", status: 200, outcome: "timeout",
});
const fail = (stage) => `PET_HTTP_DIAGNOSTIC:${JSON.stringify({ diagnostic: fixture(stage), message: "not found https://private/?key=SECRET" })}`;
async function withInvoke(invoke, fn) {
  const oldWindow = globalThis.window;
  globalThis.window = { __TAURI_INTERNALS__: { invoke } };
  try { return await fn(); } finally { globalThis.window = oldWindow; }
}

test("native timeout keeps phase, elapsed and ID, without leaking payload or falling back on 'not found'", async () => {
  let calls = 0;
  await withInvoke(async (_command, args) => {
    calls++; assert.equal(args.diagnosticStage, "thinking"); throw fail("thinking");
  }, async () => {
    await assert.rejects(pipelineFetch("https://private/?token=SECRET", { method: "POST", diagnosticStage: "thinking" }), (err) => {
      assert.match(err.message, /已收到服务响应.*读取内容超时/s);
      assert.match(err.message, /120\.0 秒/);
      assert.match(err.message, /req-fixture-123/);
      assert.doesNotMatch(err.message, /SECRET|https:|not found|公司/);
      assert.equal(err.diagnostic.phase, "response_body"); return true;
    });
  });
  assert.equal(calls, 1);
});

test("successful HTTP bridge preserves HTTP status and request ID for provider errors", async () => {
  await withInvoke(async () => ({status:404, body:'{"error":{"code":"InvalidEndpointOrModel.NotFound"}}', diagnostic:{...fixture(), outcome:"http_error", status:404}}), async () => {
    const response = await pipelineFetch("https://fixture.test", { diagnosticStage: "thinking" });
    await assert.rejects(readJsonOrThrow(response, "thinking model"), (err) => {
      assert.equal(err.status,404); assert.equal(err.diagnostic.requestId,"req-fixture-123"); return true;
    });
  });
});

test("image, thinking and video submissions never duplicate an unknown transport outcome", async () => {
  for (const [stage, run] of [
    ["thinking", () => callThinkingModel({thinking:{apiKey:"fixture-key"},image:{dataUrl:"data:image/png;base64,AA=="}})],
    ["image_edit", () => editImageBackgroundWithArk({apiKey:"fixture-key",imageDataUrl:"data:image/png;base64,AA=="})],
    ["video_submit", () => runVolcanoFamily({config:{apiKey:"fixture-key",model:"fixture"},prompt:"test",imageDataUrl:"data:image/png;base64,AA=="})],
  ]) {
    let calls = 0;
    await withInvoke(async (_command, args) => { calls++; assert.equal(args.diagnosticStage, stage); throw fail(stage); }, async () => {
      await assert.rejects(run(), /云服务请求失败/);
    });
    assert.equal(calls,1,stage);
  }
});

test("journal is allowlisted, bounded and survives storage readback", () => {
  const old = globalThis.localStorage; let stored = "";
  globalThis.localStorage = {getItem:()=>stored,setItem:(_key,value)=>{stored=value;}};
  try {
    for(let i=0;i<110;i++) recordDiagnostic({...fixture(), localId:`test-${i}`, requestId:"https://private/key", apiKey:"SECRET", body:"PRIVATE_IMAGE", url:"INTERNAL_PATH"});
    const records = getGenerationDiagnostics();
    assert.equal(records.length,100);
    assert.equal(records.at(-1).requestId,null);
    assert.doesNotMatch(stored,/SECRET|PRIVATE_IMAGE|INTERNAL_PATH|https:/);
    assert.match(formatDiagnostic(records.at(-1)), /请求编号：未取得/);
    assert.equal(parseNativeDiagnostic("not-json"),null);
  } finally { globalThis.localStorage = old; }
});
