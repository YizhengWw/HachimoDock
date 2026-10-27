import {test} from "node:test";
import assert from "node:assert/strict";
import {inspectDraft,parseDraft} from "./rewrite-knowledge-corpus.mjs";
test("requires enough Chinese characters, complete output and a question",()=>{
  assert.deepEqual(inspectDraft({title:"为什么？",text:"知".repeat(360)+"。"}).errors,[]);
  assert.ok(inspectDraft({title:"为什么？",text:"x".repeat(400)+"。"}).errors.includes("length"));
  assert.ok(inspectDraft({title:"为什么？",text:"知".repeat(360)+"。"},"length").errors.includes("truncated"));
  assert.ok(inspectDraft({title:"知识",text:"知".repeat(360)}).errors.includes("incomplete"));
});
test("safely handles adjacent notes but refuses incomplete/extra content",()=>{
  assert.deepEqual(parseDraft('{"title":"为什么？","text":"内容"}\n{"reviewNotes":"待核"}'),{title:"为什么？",text:"内容",reviewNotes:"待核"});
  assert.throws(()=>parseDraft('{"title":"为什么？","text":"内容"}\n{"text":"替换"}'));
  assert.throws(()=>parseDraft('{"title":"为什么？"}\n{"reviewNotes":'));
  assert.throws(()=>parseDraft('hello {"title":"为什么？"}'));
});
