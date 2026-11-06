"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const { CodexUserInputTracker } = require("../agents/codex-user-input");
const CodexLogMonitor = require("../agents/codex-log-monitor");
const defaults = require("../agents/codex");
const request = (id, count = 1, name = "request_user_input_async") => ({
  type: "function_call", name, call_id: id,
  arguments: JSON.stringify({questions:Array.from({length:count},(_,i)=>({title:"Question "+i}))}),
});
const answer = (id, index) => ({message:"<send_user_message_question_reply>"+
  JSON.stringify([{questionItemId:JSON.stringify(["request_user_input_async",id,index]),answer:"ok"}])+
  "</send_user_message_question_reply>"});

test("async questions wait only after acceptance and retire only matching replies", () => {
  const t = new CodexUserInputTracker();
  t.observe("response_item:function_call",request("a",2));
  assert.equal(t.waiting,false);
  t.observe("response_item:function_call_output",{call_id:"a",output:'{"accepted":true}'});
  assert.equal(t.waiting,true);
  t.observe("event_msg:user_message",{message:"An unrelated steering message"});
  t.observe("event_msg:user_message",answer("other",0));
  assert.equal(t.waiting,true);
  t.observe("event_msg:user_message",answer("a",0));
  assert.equal(t.waiting,true);
  t.observe("event_msg:user_message",answer("a",1));
  assert.equal(t.waiting,false);
  t.observe("response_item:function_call",request("a"));
  t.observe("response_item:function_call_output",{call_id:"a",output:'{"accepted":true}'});
  assert.equal(t.waiting,false,"old call receipts cannot resurrect answered questions");
});
test("failed async requests and synchronous results do not leave stale waits", () => {
  const t = new CodexUserInputTracker();
  t.observe("response_item:function_call",request("a"));
  t.observe("response_item:function_call_output",{call_id:"a",output:'{"accepted":false}'});
  assert.equal(t.waiting,false);
  t.observe("response_item:function_call",request("b",1,"functions.request_user_input"));
  assert.equal(t.waiting,true);
  t.observe("response_item:function_call_output",{call_id:"b",output:'{"answers":{}}'});
  assert.equal(t.waiting,false);
});
test("terminal lifecycle clears user input without affecting unrelated tools", () => {
  const t = new CodexUserInputTracker();
  t.observe("response_item:function_call",request("a",1,"search"));
  assert.equal(t.waiting,false);
  t.observe("response_item:function_call",request("b",1,"request_user_input"));
  t.observe("event_msg:task_complete",{});
  assert.equal(t.waiting,false);
});
test("Codex monitor preserves accepted waits across tools and assistant progress", () => {
  const events=[];
  const m=new CodexLogMonitor({...defaults,SESSION_INDEX_PATH:""},
    (id,state,event,extra)=>events.push({state,event,content:extra.display.content}));
  m.getIndexedTitle=()=>"StopWatch user input test";
  const entry=m.createEntry("codex:test",0,false);
  const feed=(type,payload)=>m.processLine(JSON.stringify({type,payload}),entry);
  feed("response_item",request("a"));
  feed("response_item",{type:"function_call_output",call_id:"a",output:'{"accepted":true}'});
  feed("response_item",{type:"custom_tool_call",name:"exec_command",input:"read-only work"});
  feed("response_item",{type:"message",role:"assistant",content:[{type:"output_text",text:"Independent progress"}]});
  assert.equal(events.at(-1).state,"notification");
  assert.equal(events.at(-1).content,"等待你的确认");
  feed("response_item",{type:"message",role:"user",content:[{type:"input_text",text:answer("a",0).message}]});
  assert.equal(events.at(-1).state,"thinking");
  feed("event_msg",{type:"task_complete",last_agent_message:"done"});
  assert.equal(events.at(-1).state,"attention");
});

test("real Codex task_complete.error is failure, not successful completion", () => {
  const events=[];
  const m=new CodexLogMonitor({...defaults,SESSION_INDEX_PATH:""},
    (id,state,event,extra)=>events.push({state,content:extra.display.content}));
  m.getIndexedTitle=()=>"StopWatch error test";
  const entry=m.createEntry("codex:error-test",0,false);
  m.processLine(JSON.stringify({type:"event_msg",payload:{type:"task_started"}}),entry);
  m.processLine(JSON.stringify({type:"event_msg",payload:{type:"task_complete",
    last_agent_message:null,error:{message:"controlled 503",
      codex_error_info:{http_connection_failed:{http_status_code:503}}}}}),entry);
  assert.equal(events.at(-1).state,"error");
  assert.notEqual(events.at(-1).content,"已完成");
  m.processLine(JSON.stringify({type:"event_msg",payload:{type:"task_started"}}),entry);
  m.processLine(JSON.stringify({type:"event_msg",payload:{type:"task_complete",
    last_agent_message:"success",error:null}}),entry);
  assert.equal(events.at(-1).state,"attention");
});

test("an indexed thread name replaces an earlier generic monitor title", () => {
  const m = new CodexLogMonitor({...defaults, SESSION_INDEX_PATH:""}, () => {});
  let indexed = "";
  m.getIndexedTitle = () => indexed;
  const entry = m.createEntry("codex:renamed-test", 0, false);
  const generic = m.ensureTitle(entry);
  indexed = "StopWatch 联调错误分支";
  assert.notEqual(generic, indexed);
  assert.equal(m.ensureTitle(entry), indexed);
  indexed = "";
  assert.equal(m.ensureTitle(entry), "StopWatch 联调错误分支", "a transient missing index retains the known title");
});
