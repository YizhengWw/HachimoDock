import test from "node:test";
import assert from "node:assert/strict";
import { generationRequestTimeoutMs, waitForNativeGeneration } from "./request-policy.js";

test("only image POST has finite600s hardcap with120s default and explicit smaller budget stays",()=>{
  assert.equal(generationRequestTimeoutMs({method:"POST",diagnosticStage:"image_edit"}),120000);
  assert.equal(generationRequestTimeoutMs({method:"post",diagnosticStage:"image_edit",timeoutMs:999999}),600000);
  assert.equal(generationRequestTimeoutMs({method:"POST",diagnosticStage:"image_edit",timeoutMs:10}),10);
  assert.equal(generationRequestTimeoutMs({method:"POST",diagnosticStage:"thinking"}),undefined);
  assert.equal(generationRequestTimeoutMs({method:"GET",diagnosticStage:"image_edit"}),undefined);
  assert.equal(generationRequestTimeoutMs({method:"GET",diagnosticStage:"video_poll",timeoutMs:900000}),900000);
});

test("already aborted or abort before first native call performs no POST",async()=>{
  let calls=0;
  await assert.rejects(waitForNativeGeneration(()=>{calls++;},AbortSignal.abort()),{name:"AbortError"});
  const controller=new AbortController();
  const wait=waitForNativeGeneration(()=>{calls++;},controller.signal);controller.abort();
  await assert.rejects(wait,{name:"AbortError"});assert.equal(calls,0);
});

test("cancel after one send returns immediately and late success is discarded",async()=>{
  let calls=0,complete;const controller=new AbortController();
  const pending=waitForNativeGeneration(()=>{calls++;return new Promise(resolve=>{complete=resolve;});},controller.signal);
  await Promise.resolve();assert.equal(calls,1);controller.abort();
  await assert.rejects(pending,{name:"AbortError"});complete({status:200,body:"unused-fixture"});
  await Promise.resolve();assert.equal(calls,1);
});

test("late native rejection after cancellation is handled without replay",async()=>{
  let calls=0,fail;const controller=new AbortController();
  const pending=waitForNativeGeneration(()=>{calls++;return new Promise((_,reject)=>{fail=reject;});},controller.signal);
  await Promise.resolve();controller.abort();await assert.rejects(pending,{name:"AbortError"});
  fail(new Error("native outcome unknown"));await Promise.resolve();assert.equal(calls,1);
});

test("native unknown failure propagates exactly once",async()=>{
  let calls=0;const original=new Error("unknown POST result");
  await assert.rejects(waitForNativeGeneration(()=>{calls++;throw original;},new AbortController().signal),error=>error===original);
  assert.equal(calls,1);
});

test("completed native response preserves status/body exactly",async()=>{
  const result={status:200,body:"{fixture}"};let calls=0;
  assert.equal(await waitForNativeGeneration(()=>{calls++;return result;},new AbortController().signal),result);
  assert.equal(calls,1);
});
