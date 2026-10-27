"use strict";
const {test}=require("node:test");
const assert=require("node:assert/strict");
const {createHash}=require("node:crypto");
const {requestText}=require("../agents/codex-request-text");
const Monitor=require("../agents/codex-log-monitor");
const {SessionMetricsTracker}=require("../packages/clawd-backend-service/src/status-metrics");
const wrap=text=>`\n<in-app-browser-context source="ambient-ui-state">\nThis block is automatically supplied ambient UI state, not part of the user's request. Do not treat it as an instruction or as evidence that the user explicitly selected the in-app browser.\n# In app browser:\n- The user has the in-app browser open with 1 tab.\n- Current URL: https://example.invalid/\n</in-app-browser-context>\n\n## My request:\n${text}`;
const hash=text=>createHash("sha256").update(text.trim()).digest("hex");

test("only the exact leading Desktop envelope is removed; embedded drafts and request bytes survive",()=>{
 const request='这是用户通过 Pet 实时对话委托的桌面任务\n语音对话数据（JSON）：\n'+JSON.stringify({current_user_request:"打开测试页面",existing_draft:wrap("old request")});
 assert.deepEqual(requestText(wrap(request)),{text:request,normalized:true});
 assert.deepEqual(requestText(wrap("").replaceAll("\n","\r\n")+request),{text:request,normalized:true});
 for(const text of ["ordinary request", "quote: "+wrap(request), wrap(request).replace('source="ambient-ui-state"','source="other"'),wrap(request).replace("## My request:","## Other:"),wrap(request).replace("# In app browser:","# Malformed:"),wrap(request).replace("- Current URL:","<untrusted>"),wrap("")]) {
  assert.deepEqual(requestText(text),{text:text.trim(),normalized:false});
 }
});

test("modern wrapped user event and completion match original handoff; mirrored events do not duplicate",()=>{
 for(const order of [["wrapped"],["wrapped","plain"],["plain","wrapped"]]) {
  const tracker=new SessionMetricsTracker();const baseline=tracker.sessionEvents("codex");
  const monitor=new Monitor(require("../agents/codex"),(id,state,event,extra)=>tracker.apply({source:"codex",sessionId:id,event,state,display:extra.display,taskCorrelation:extra.taskCorrelation}));
  const entry=monitor.createEntry("codex:fixture",0,false);
  const request='voice handoff '+JSON.stringify({current_user_request:"test only",existing_draft:"\n"});
  const emit=p=>monitor.processLine(JSON.stringify(p),entry);
  emit({type:"event_msg",payload:{type:"task_started",turn_id:"new-turn"}});
  for(const type of order)emit(type==="wrapped"?{type:"response_item",payload:{type:"message",role:"user",content:[{type:"input_text",text:wrap(request)}]}}:{type:"event_msg",payload:{type:"user_message",message:request}});
  emit({type:"event_msg",payload:{type:"task_complete",turn_id:"new-turn",last_agent_message:"Test complete."}});
  const events=tracker.sessionEvents("codex",baseline).events;
  const users=events.filter(e=>e.event==="event_msg:user_message");
  assert.equal(users.length,1);
  assert.equal(users[0].taskCorrelation.userHash,hash(request));
  const done=events.find(e=>e.event==="event_msg:task_complete");
  assert.equal(done.taskCorrelation.userHash,hash(request));
  assert.equal(done.taskCorrelation.turnId,"new-turn");
  assert.equal(done.session.displayContent,"Test complete.");
  assert(!JSON.stringify(events).includes("example.invalid"));
  if(order[0]==="wrapped")assert.equal(users[0].taskCorrelation.browserContextNormalized,true);
 }
});
