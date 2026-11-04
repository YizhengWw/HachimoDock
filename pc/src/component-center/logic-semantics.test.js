import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {initialApp, appAction, appAdvance} from './app-runtime.js';
import * as packaged from '../../skills/petui/scripts/app-runtime.mjs';
import {createSceneCore} from './scene-core.js';

const suite=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/logic-v1.json',import.meta.url)));
assert.equal(suite.profile,'p4-logic-v1');
for(const [port,model] of [['preview',{initialApp,appAction,appAdvance}],['packaged',packaged]])
for(const item of suite.cases)test(`logic contract (${port}): ${item.name}`,()=>{
 let state=model.initialApp(item.runtime);
 for(const event of item.events)state=event.action?model.appAction(item.runtime,state,event.action):model.appAdvance(item.runtime,state,event.ms);
 assert.deepEqual({state:state.state,page:state.page,vars:state.vars},item.expected);
});

// Scene side of p4-logic-v1 through the real shared C core: rule guards read runtime
// vars, var ops write them back, and restart never resets the mirrored vars.
const bytes=readFileSync(new URL('./classic-core.wasm',import.meta.url));
const vars={ammo:{type:'int',init:2},hits:{type:'int',init:0}};
const scene={tick_ms:300,active_state:'play',result_state:'over',score_var:'hits',grid:{width:8,height:8},
 entities:[{id:'player',x:3,y:6},{id:'bullet',x:3,y:5,vy:-2,active:false,bounds:'hide',shape:'bullet'},{id:'enemy',x:3,y:1,shape:'enemy-ship'}],
 rules:[
  {on:'start',do:[{op:'restart'}]},
  {on:'fire',when:{var:'ammo',op:'gt',value:0},do:[{op:'place',entity:'bullet',source:'player',dy:-1},{op:'var',name:'ammo',inc:-1}]},
  {on:'collision',entity:'bullet',with:'enemy',do:[{op:'hide',entity:'enemy'},{op:'hide',entity:'bullet'},{op:'score',add:1}]}
 ]};
test('scene core guards fire on vars and var ops write back',async()=>{
 const c=await createSceneCore(bytes,scene,vars);
 c.action('start',0);
 c.setVars({ammo:2,hits:0});c.action('fire',10);assert.equal(c.readVars().ammo,1);assert.equal(c.snapshot().entities[1].active,true);
 c.tick(310);c.tick(610);c.tick(910);
 assert.equal(c.snapshot().score,1,'first tick arms the clock, then bullet 5→3→1 meets the enemy at row 1');
 c.setVars({ammo:1,hits:1});c.action('fire',1000);assert.equal(c.readVars().ammo,0);
 c.setVars({ammo:0,hits:1});const before=c.snapshot();c.action('fire',1100);
 assert.deepEqual(c.snapshot().entities,before.entities,'guarded rule must not place a bullet without ammo');
 assert.equal(c.readVars().ammo,0);
 c.action('start',1200);assert.equal(c.readVars().ammo,0,'restart does not reset mirrored vars');
});
test('scene core without the profile ignores vars and keeps the legacy op table',async()=>{
 const legacy={...scene,rules:[{on:'start',do:[{op:'restart'}]},{on:'fire',do:[{op:'place',entity:'bullet',source:'player',dy:-1}]}]};
 const c=await createSceneCore(bytes,legacy);
 c.action('start',0);c.action('fire',1);assert.equal(c.snapshot().entities[1].active,true);
 assert.deepEqual(c.readVars(),{});
});
