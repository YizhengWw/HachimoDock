"use strict";
const assert=require("node:assert/strict");
const {test}=require("node:test");
const {createHash}=require("node:crypto");
const CodexLogMonitor=require("../agents/codex-log-monitor");
const {SessionMetricsTracker}=require("../packages/clawd-backend-service/src/status-metrics");
const {stateForHook}=require("../hooks/workbuddy-pet-manager-hook");

test("WorkBuddy hook receipts bind each generation without leaking prompts or borrowing other turns",()=>{
  const tracker=new SessionMetricsTracker(); const baseline=tracker.sessionEvents("workbuddy");
  const emit=(event,generation,prompt,session="test")=>{
    const payload=stateForHook({hook_event_name:event,session_id:session,generation_id:generation,prompt});
    tracker.apply({source:"workbuddy",sessionId:session,event:payload.event,state:payload.state,
      taskCorrelation:payload.task_correlation});
  };
  emit("UserPromptSubmit","generation-a","first private task");
  emit("UserPromptSubmit","generation-b","second private task");
  emit("Stop","generation-a");
  emit("Stop","generation-b");
  emit("Stop",undefined);
  emit("Stop","generation-a",undefined,"other-session");
  emit("UserPromptSubmit",undefined,"legacy private task");
  emit("Stop",undefined);
  const events=tracker.sessionEvents("workbuddy",baseline).events;
  const hash=s=>createHash("sha256").update(s).digest("hex");
  assert.equal(events[2].taskCorrelation.userHash,hash("first private task"));
  assert.equal(events[3].taskCorrelation.userHash,hash("second private task"));
  for(const i of [4,5,7])assert.equal(events[i].taskCorrelation.userHash,"");
  assert.equal(events[6].taskCorrelation.userHash,hash("legacy private task"));
  assert.equal(events[6].taskCorrelation.turnId,"");
  assert.ok(!JSON.stringify(events).includes("private task"));
  assert.ok(!JSON.stringify(events).includes("generation-a"));
  tracker.apply({source:"workbuddy",sessionId:"test",event:"Stop",state:"done"});
  assert.equal(tracker.sessionEvents("workbuddy",baseline).events.at(-1).taskCorrelation,undefined);
});

test("modern Desktop response_item user messages produce exact receipts and mirrored logs deduplicate",()=>{
  for (const order of [["response_item"], ["response_item","event_msg"], ["event_msg","response_item"]]) {
    const events=[];
    const monitor=new CodexLogMonitor(require("../agents/codex"),(id,state,event,extra)=>events.push({event,extra}));
    const entry=monitor.createEntry("codex:modern",0,false);
    const send=(type,payload)=>monitor.processLine(JSON.stringify({type,payload}),entry);
    const request="用 ChatGPT 给我发送一个你好。";
    for (let turn=1;turn<=2;turn++) {
      send("event_msg",{type:"task_started",turn_id:`turn-${turn}`});
      assert.equal(events.at(-1).extra.taskCorrelation.userHash, "");
      for (const type of order) send(type,type==="event_msg"?{type:"user_message",message:request}:{
        type:"message",role:"user",content:[{type:"input_text",text:request},{type:"input_image",image_url:"not text"}],
      });
      send("response_item",{type:"message",role:"assistant",content:[{type:"output_text",text:"你好！"}]});
      send("event_msg",{type:"task_complete",turn_id:`turn-${turn}`});
      assert.equal(events.at(-1).extra.taskCorrelation.userHash,createHash("sha256").update(request).digest("hex"));
      assert.equal(events.at(-1).extra.taskCorrelation.turnId,`turn-${turn}`);
    }
    assert.equal(entry.userMessageCount,2);
    assert.equal(events.filter(e=>e.event==="event_msg:user_message").length,2);
    for (const role of ["developer","system","tool"]) send("response_item",{type:"message",role,content:[{type:"input_text",text:request}]});
    send("response_item",{type:"message",role:"user",content:[{type:"input_image",image_url:"not text"}]});
    assert.equal(entry.userMessageCount,2);
  }
});

test("legacy user-before-start ordering retains the fresh hash",()=>{
  const monitor=new CodexLogMonitor(require("../agents/codex"),()=>{});
  const entry=monitor.createEntry("codex:legacy",0,false);
  for (let turn=1;turn<=2;turn++) {
    for(const payload of [{type:"user_message",message:"new request"},{type:"task_started",turn_id:`turn-${turn}`},{type:"task_complete",turn_id:`turn-${turn}`}])
      monitor.processLine(JSON.stringify({type:"event_msg",payload}),entry);
    assert.equal(entry.lastUserHash,createHash("sha256").update("new request").digest("hex"));
  }
});

test("steered user receipts carry active-turn evidence while queued next-turn receipts do not",()=>{
  const tracker=new SessionMetricsTracker();const baseline=tracker.sessionEvents("codex");
  const monitor=new CodexLogMonitor(require("../agents/codex"),(id,state,event,extra)=>{
    tracker.apply({source:"codex",sessionId:id,event,state,taskCorrelation:extra.taskCorrelation});
  });
  const entry=monitor.createEntry("codex:insertion",0,false);
  const send=payload=>monitor.processLine(JSON.stringify({type:"event_msg",payload}),entry);
  send({type:"task_started",turn_id:"running"});
  send({type:"user_message",message:"original"});
  send({type:"user_message",message:"additional"});
  send({type:"task_complete",turn_id:"running",last_agent_message:"reply"});
  send({type:"user_message",message:"queued next"});
  send({type:"task_started",turn_id:"next"});
  const events=tracker.sessionEvents("codex",baseline).events;
  const users=events.filter(e=>e.event==="event_msg:user_message");
  assert.deepEqual(users.map(e=>e.taskCorrelation.turnActive),[true,true,false]);
  assert.equal(users[1].taskCorrelation.turnId,"running");
  assert.equal(events.find(e=>e.event==="event_msg:task_complete").taskCorrelation.userHash,
    createHash("sha256").update("additional").digest("hex"));
  assert.equal(events.at(-1).taskCorrelation.userHash,createHash("sha256").update("queued next").digest("hex"));
  assert.equal(events.at(-1).taskCorrelation.turnId,"next");
});

test("voice task correlation survives real monitor and ordered metrics without exporting full prompts",()=>{
  const tracker=new SessionMetricsTracker();
  const baseline=tracker.sessionEvents("codex");
  const events=[];
  const monitor=new CodexLogMonitor({SESSION_INDEX_PATH:"",LOG_EVENT_MAP:{
    "event_msg:task_started":"thinking","event_msg:user_message":"thinking",
    "event_msg:agent_message":"attention","event_msg:task_complete":"attention",
  }},(id,state,event,extra)=>{
    events.push(extra);
    tracker.apply({source:"codex",sessionId:id,event,state:state==="attention"?"done":state,
      display:event==="event_msg:user_message"?undefined:extra.display,taskCorrelation:extra.taskCorrelation});
  });
  const entry=monitor.createEntry("codex:voice-test",0,false);
  const request="让 ChatGPT 帮我写总结。"+"详情。".repeat(150);
  for(const payload of [
    {type:"task_started",turn_id:"voice-turn-1"},
    {type:"user_message",message:request},
    {type:"agent_message",message:"总结已经完成。"},
    {type:"task_complete",turn_id:"voice-turn-1",last_agent_message:"总结已经完成。"},
  ])monitor.processLine(JSON.stringify({type:"event_msg",payload}),entry);
  const batch=tracker.sessionEvents("codex",baseline);
  assert.equal(batch.reset,false);
  const user=batch.events.find(e=>e.event==="event_msg:user_message");
  const done=batch.events.find(e=>e.event==="event_msg:task_complete");
  const expected=createHash("sha256").update(request.trim()).digest("hex");
  assert.equal(user.taskCorrelation.userHash,expected);
  assert.equal(done.taskCorrelation.userHash,expected);
  assert.equal(done.taskCorrelation.turnId,"voice-turn-1");
  assert.equal(done.session.displayContent,"总结已经完成。");
  assert.ok(!JSON.stringify(batch).includes(request));
  assert.ok(events.some(e=>e.messages.lastUser.length<request.length));
});

test("correlation changes are events even when state and previews are identical",()=>{
  const tracker=new SessionMetricsTracker();const baseline=tracker.sessionEvents("codex");
  for(const hash of ["a".repeat(64),"b".repeat(64)])tracker.apply({source:"codex",sessionId:"same",
    state:"thinking",event:"event_msg:user_message",display:{content:"正在思考"},
    taskCorrelation:{userHash:hash,turnId:"turn-a"}});
  const batch=tracker.sessionEvents("codex",baseline);
  assert.deepEqual(batch.events.map(e=>e.taskCorrelation.userHash),["a".repeat(64),"b".repeat(64)]);
});

test("correlation fields are bounded metadata and never accepted from other agents",()=>{
  const tracker=new SessionMetricsTracker();const baseline=tracker.sessionEvents("codex");
  tracker.apply({source:"codex",sessionId:"same",state:"thinking",event:"event_msg:user_message",
    taskCorrelation:{userHash:"raw private task",turnId:"/private/path",prompt:"never export"}});
  const correlation=tracker.sessionEvents("codex",baseline).events[0].taskCorrelation;
  assert.deepEqual(correlation,{userHash:"",turnId:""});
  const other=tracker.sessionEvents("claude-code");
  tracker.apply({source:"claude-code",sessionId:"same",state:"working",taskCorrelation:{userHash:"a".repeat(64),turnId:"other"}});
  assert.equal(tracker.sessionEvents("claude-code",other).events[0].taskCorrelation,undefined);
});
