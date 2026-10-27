import test from "node:test";
import assert from "node:assert/strict";
import {initialApp,appAction,appAdvance,appDashboard,appHit} from "./app-runtime.js";
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
