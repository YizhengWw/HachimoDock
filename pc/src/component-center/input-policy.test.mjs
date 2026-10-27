import test from "node:test";
import assert from "node:assert/strict";
import {actionEnabled,swipeAction,pressAction} from "./input-policy.js";
import {appVisible,appHit} from "./app-runtime.js";
import {initialApp,appAction,appAdvance} from "./app-runtime.js";
const runtime={input:{engine:"p4-input-v1",press:["flap"],swipe:{left:"next",right:"prev"}},transitions:[{from:"playing",on:"flap"},{from:"*",on:"next"}]};
test("state-scoped controls cannot look actionable while inactive",()=>{
  assert.equal(actionEnabled(runtime,{state:"paused"},"flap"),false);
  assert.equal(actionEnabled(runtime,{state:"playing"},"flap"),true);
  assert.equal(actionEnabled(runtime,{state:"paused"},"next"),true);
  assert.equal(actionEnabled({},{},"legacy"),true);
});
test("swipe requires minimum length, one dominant axis and bounded time",()=>{
  assert.equal(swipeAction(runtime,-120,10,200),"next");
  assert.equal(swipeAction(runtime,90,-10,200),"prev");
  for(const [x,y,t] of [[10,0,100],[90,80,100],[90,0,800],[90,0,-1],[0,100,300]])assert.equal(swipeAction(runtime,x,y,t),null);
});
test("only explicit immediate actions fire at press",()=>{
  assert.equal(pressAction(runtime,"flap"),true);
  assert.ok(!pressAction(runtime,"next"));
});
test("host picker visibility and eligibility use the same snapshot",()=>{
  const n={type:"button",x:0,y:0,w:100,h:48,action:"next",host_mask:8};
  assert.equal(appVisible(n,0,{},0,0),false);
  assert.equal(appHit({elements:[n]},0,20,20,{},0,0),null);
  assert.equal(appHit({elements:[n]},0,20,20,{},0,3),"next");
  assert.equal(actionEnabled({...runtime,host:true},{state:"idle",ui:{mode:0,actions:[]}},"next"),false);
  assert.equal(actionEnabled({...runtime,host:true},{state:"idle",ui:{mode:3,actions:["next"]}},"next"),true);
});
test("data paging is inactive for empty/single pages and a redundant home action",()=>{
  const r={input:{engine:"p4-input-v1"},data:{page_var:"p"},transitions:[
    {from:"*",on:"next",inc:{p:1}},{from:"*",on:"prev",inc:{p:-1}},{from:"*",on:"home",set:{p:0}}]};
  for(const count of [0,1,5,6,11,16,20]){
    const s={state:"view",vars:{p:0},data:{rows:Array.from({length:count},()=>({}))}};
    assert.equal(actionEnabled(r,s,"home"),false);
    assert.equal(actionEnabled(r,s,"next"),count>5);
    assert.equal(actionEnabled(r,s,"prev"),count>5);
    s.vars.p=-1;assert.equal(actionEnabled(r,s,"home"),count>5);
    s.data.rows=[];assert.equal(actionEnabled(r,s,"home"),false);
  }
});
test("care cooldown starts at time zero and touch/key actions share it",()=>{
  const r={input:{engine:"p4-input-v1",cooldown:{feed:30000}},vars:{bond:{type:"int",init:0}},
    states:["idle"],initial_state:"idle",transitions:[{from:"*",on:"feed",inc:{bond:2}}]};
  let s=appAction(r,initialApp(r),"feed");assert.equal(s.vars.bond,2);
  for(let i=0;i<50;i++)s=appAction(r,s,"feed");
  assert.equal(s.vars.bond,2);
  s=appAdvance(r,s,29999);assert.equal(actionEnabled(r,s,"feed"),false);
  s=appAdvance(r,s,1);assert.equal(actionEnabled(r,s,"feed"),true);
  s=appAction(r,s,"feed");assert.equal(s.vars.bond,4);
});
