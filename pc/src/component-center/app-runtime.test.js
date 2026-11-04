import test from "node:test";
import assert from "node:assert/strict";
import {initialApp,appAction,appAdvance,appDashboard,appHit,appNodeValue,appNodeMeter,takeCues} from "./app-runtime.js";
const r={initial_state:"idle",initial_page:"home",pages:[{id:"home"},{id:"garden"}],states:["idle","run","done"],vars:{left:{type:"int",init:3},total:{type:"int",init:3},count:{type:"int",init:0}},transitions:[{from:"idle",on:"start",to:"run"},{from:"done",on:"start",to:"run",set:{left:3}},{from:"*",on:"next",page:"next"}],tick:[{every_ms:1000,while_state:"run",inc:{left:-1},when:{var:"left",lte:0},then:{to:"done",inc:{count:1}}}],dashboard:{headline:{switch_state:{idle:"开始",run:"专注",done:"完成"}},metricValue:{fmt_mmss:"left"},badge:{var:"count"},progress:{pct_of:"left",of_max:"total"}}};
test("bounded timer settles once, restart preserves collection",()=>{
 let s=initialApp(r);s=appAdvance(r,s,1000);assert.equal(s.vars.left,3);
 s=appAction(r,s,"start");s=appAdvance(r,s,3000);assert.equal(s.state,"done");assert.equal(s.vars.count,1);
 assert.equal(appDashboard(r,s).metricValue,"00:00");s=appAdvance(r,s,60000);assert.equal(s.vars.count,1);
 s=appAction(r,s,"start");assert.equal(s.vars.left,3);assert.equal(s.vars.count,1);
 assert.equal(appAction(r,s,"unknown"),s);
});
test("page wrap and topmost touch controls match firmware",()=>{
 let s=appAction(r,initialApp(r),"next");assert.equal(s.page,"garden");s=appAction(r,s,"next");assert.equal(s.page,"home");
 const surface={elements:[{x:0,y:0,w:50,h:50,page:4,action:"a"},{x:10,y:10,w:20,h:20,page:1,action:"b"}]};
 assert.equal(appHit(surface,0,15,15),"a");assert.equal(appHit(surface,1,15,15),"b");assert.equal(appHit(surface,0,50,50),null);
});
test("omitted while_state runs globally; named ticks only run in that state",()=>{
 const runtime={...r,vars:{count:{type:"int",init:0}},tick:[{every_ms:1000,inc:{count:1}}]};
 let s=appAdvance(runtime,initialApp(runtime),10000);
 assert.equal(s.vars.count,10);
 s=appAction(runtime,s,"start");s=appAdvance(runtime,s,1000);
 assert.equal(s.vars.count,11);
 const named={...runtime,tick:[{every_ms:1000,while_state:"run",inc:{count:1}}]};
 let n=appAdvance(named,initialApp(named),5000);assert.equal(n.vars.count,0);
 n=appAction(named,n,"start");n=appAdvance(named,n,2000);assert.equal(n.vars.count,2);
});
test('named bindings render independently from legacy dashboard slots',()=>{
 const s={vars:{'custom.temperature':'24.5 °C',fill:140,zero:0}};
 assert.equal(appNodeValue({var_bind:'custom.temperature'},s,{headline:'other'}),'24.5 °C');
 assert.equal(appNodeValue({var_bind:'zero'},s),0);
 assert.equal(appNodeValue({var_bind:'missing'},s),'');
 assert.equal(appNodeValue({bind:'headline'},s,{headline:'legacy'}),'legacy');
 assert.equal(appNodeMeter({var_bind:'fill'},s),100);
 assert.equal(appNodeMeter({var_bind:'fill'},{vars:{fill:-3}}),0);
 assert.equal(appNodeMeter({},s,{progress:25}),25);
});
test('dashboard switch branches resolve like firmware: leaves only, no nested switch',()=>{
 const runtime={...r,dashboard:{headline:{switch_page:{home:{switch_state:{idle:"nested"}},garden:"B"}},metricValue:{switch_state:{idle:{var:"count"},run:"x"}},note:{text:"unsupported"},badge:{var:"count",suffix:"个"}}};
 const s=initialApp(runtime);const d=appDashboard(runtime,s);
 assert.equal(d.headline,"");          // nested switch is rejected by the device, so it renders nothing
 assert.equal(d.metricValue,"0");      // a {var} leaf inside a switch is fine
 assert.equal(d.note,"");              // unknown rule keys render nothing
 assert.equal(d.badge,"0");            // extra keys are ignored by the device parser
 assert.equal(appDashboard(runtime,appAction(runtime,s,"next")).headline,"B");
});
test("p4-sfx-v1: transitions and tick completions queue cues once per drain, beside a native game",()=>{
  const runtime={engine:"p4-bounded-runtime-v4",logic:"p4-logic-v1",sfx:{engine:"p4-sfx-v1",volume:50,cues:[{id:"tap",preset:"tap"},{id:"beep",preset:"beep"},{id:"low",preset:"low"}]},
    vars:{score:{type:"int",init:0},half:{type:"int",init:2},bgm:{type:"int",init:0}},states:["ready","playing"],initial_state:"ready",
    transitions:[{from:"ready",on:"g.place",to:"playing",sfx:"tap"},{from:"playing",on:"g.place",sfx:"tap"}],
    tick:[{every_ms:400,while_state:"playing",inc:{bgm:1},when:{var:"bgm",op:"gte",value:8},then:{set:{bgm:0},sfx:"low"}},
          {every_ms:100,while_state:"playing",when:{var:"score",op:"gte",value:{var:"half"}},then:{inc:{half:2},sfx:"beep"}}],
    game:{type:"gomoku",tick_ms:300,playing_state:"playing",result_state:"ready",score_var:"score",actions:{}}};
  let s=appAction(runtime,initialApp(runtime),"g.place");
  assert.deepEqual(takeCues(s),["tap"]);assert.deepEqual(takeCues(s),[],"drained once");
  s=appAdvance(runtime,s,400);assert.equal(s.vars.bgm,1);assert.deepEqual(takeCues(s),[]);
  s=appAdvance(runtime,{...s,vars:{...s.vars,score:2}},100);  // the engine mirrored an even stone count: white replied
  assert.deepEqual(takeCues(s),["beep"]);assert.equal(s.vars.half,4);
  s=appAdvance(runtime,s,100);assert.deepEqual(takeCues(s),[],"beep is one-shot until the next even step");
  for(let i=0;i<7;i++)s=appAdvance(runtime,s,400);
  assert.equal(s.vars.bgm,0);assert.deepEqual(takeCues(s),["low"],"loop wraps with the downbeat cue");
});
