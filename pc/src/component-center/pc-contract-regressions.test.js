import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import * as preview from './app-runtime.js';
import * as packaged from '../../skills/petui/scripts/app-runtime.mjs';
import {actionEnabled} from './input-policy.js';
import {actionEnabled as packagedEnabled} from '../../skills/petui/scripts/input-policy.mjs';
import {dataProjection,visibleRows} from './d-surface.js';
import {dataProjection as packagedProjection} from '../../skills/petui/scripts/d-surface.mjs';
import {createSceneCore} from './scene-core.js';
import {createSceneCore as packagedScene} from '../../skills/petui/scripts/scene-core.mjs';
const suite=JSON.parse(fs.readFileSync(new URL('../../skills/petui/contracts/pc-contract-regressions-v1.json',import.meta.url)));
const bytes=fs.readFileSync(new URL('./classic-core.wasm',import.meta.url));
for(const [name,create] of [['preview',createSceneCore],['packaged',packagedScene]]){
 for(const c of suite.sceneGuards)test(`scene slots ${name}: ${c.name}`,async()=>{
  const r=c.runtime;
  if(!c.expectedOk){await assert.rejects(()=>create(bytes,r.scene,r.vars),/first eight declaration slots/);return;}
  const core=await create(bytes,r.scene,r.vars);
  core.setVars(Object.fromEntries(Object.entries(r.vars).map(([k,v])=>[k,v.init])));
  core.action('go',1);assert.equal(core.snapshot().entities[0].x,c.expectedX);
 });
}
for(const [name,model,enabled] of [['preview',preview,actionEnabled],['packaged',packaged,packagedEnabled]]){
 for(const c of suite.navigation)test(`actual branch ${name}: ${c.name}`,()=>{
  const state={...model.initialApp(c.runtime),history:[...c.initialHistory]};
  const before=structuredClone(state);
  assert.equal(enabled(c.runtime,state,c.action),c.expected.enabled);
  const next=model.appAction(c.runtime,state,c.action);
  assert.deepEqual({enabled:c.expected.enabled,page:next.page,hits:next.vars.hits,history:next.history||[]},c.expected);
  assert.deepEqual(state,before);if(!c.expected.enabled)assert.equal(next,state);
 });
 for(const c of suite.dataPaging)test(`filtered paging ${name}: ${c.name}`,()=>{
  const state={...model.initialApp(c.runtime),data:{rows:c.rows}};
  const projection=dataProjection(c.runtime,state);
  assert.deepEqual(projection,packagedProjection(c.runtime,state));
  assert.deepEqual(projection.rows,visibleRows(c.runtime,state));
  assert.deepEqual({count:projection.rows.length,pages:projection.pages,page:projection.page},
    {count:c.expected.count,pages:c.expected.pages,page:c.expected.page});
  assert.equal(enabled(c.runtime,state,c.action),c.expected.enabled);
  const next=model.appAction(c.runtime,state,c.action);assert.equal(next.vars.page,c.expected.storedPage);
  if(!c.expected.enabled)assert.equal(next,state);
 });
}
test('paging prediction resolves variable operands and actual clamp before admitting',()=>{
 const r={input:{engine:'p4-input-v1'},vars:{page:{type:'int',init:0},delta:{type:'int',init:2}},initial_state:'idle',states:['idle'],data:{page_var:'page'},
  transitions:[{from:'*',on:'next',inc:{page:{var:'delta'}}},{from:'*',on:'next',inc:{delta:1}}]};
 const s={...preview.initialApp(r),data:{rows:Array.from({length:6},()=>({}))}};
 assert.equal(actionEnabled(r,s,'next'),false);assert.equal(preview.appAction(r,s,'next'),s);
 s.vars.delta=1;assert.equal(actionEnabled(r,s,'next'),true);assert.equal(preview.appAction(r,s,'next').vars.page,1);
 r.limits={page:[0,0]};assert.equal(actionEnabled(r,s,'next'),false);assert.equal(preview.appAction(r,s,'next'),s);
});
test('same-action admission follows the first selected branch, not a later convenient one',()=>{
 const r={input:{engine:'p4-input-v1'},vars:{p:{type:'int',init:0},hits:{type:'int',init:0}},initial_state:'idle',states:['idle'],data:{page_var:'p'},
  transitions:[{from:'*',on:'next',inc:{p:1}},{from:'*',on:'next',inc:{hits:1}}]};
 const s={...preview.initialApp(r),data:{rows:[]}};assert.equal(actionEnabled(r,s,'next'),false);assert.equal(preview.appAction(r,s,'next'),s);
});
