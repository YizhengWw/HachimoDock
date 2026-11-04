import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {initialApp,appAction} from './app-runtime.js';
import {actionEnabled} from './input-policy.js';
import {navigationAllowed,navigate,NAVIGATION_ENGINE} from './app-navigation.js';
import * as mirror from '../../skills/petui/scripts/app-runtime.mjs';
const suite=JSON.parse(fs.readFileSync(new URL('../../skills/petui/contracts/app-navigation-v1.json',import.meta.url)));
for(const c of suite.cases)test(`navigation: ${c.name}`,()=>{
  let state=initialApp(suite.runtime);
  let packaged=mirror.initialApp(suite.runtime);
  for(const action of c.actions){state=appAction(suite.runtime,state,action);packaged=mirror.appAction(suite.runtime,packaged,action);assert.deepEqual(packaged,state);}
  assert.equal(state.page,c.page);assert.deepEqual(state.history||[],c.history);assert.equal(state.vars.hits,c.hits);
});
test('navigation disables root/back, wrong page and full stack consistently',()=>{
  const r=suite.runtime;let s=initialApp(r);assert.equal(actionEnabled(r,s,'back'),false);
  s=appAction(r,appAction(r,s,'open'),'open');assert.equal(actionEnabled(r,s,'open'),false);
  for(let i=0;i<16;i++)s=appAction(r,s,'push');assert.equal(actionEnabled(r,s,'push'),false);
});
test('old string page navigation retains legacy shape/semantics',()=>{
  const r={...suite.runtime};delete r.navigation;r.transitions=[{from:'*',on:'next',page:'next'}];
  const s=appAction(r,initialApp(r),'next');assert.equal(s.page,'list');assert.equal(Object.hasOwn(s,'history'),false);
});
test('page away/back in one render retains pointer invalidation without persisting history',()=>{
 const runtime=suite.runtime;const original=initialApp(runtime);
 const state=appAction(runtime,appAction(runtime,original,'open'),'back');
 assert.equal(state.page,original.page);assert.deepEqual(state.history,[]);assert.equal(state.pointerEpoch,2);
 const other=mirror.appAction(runtime,mirror.appAction(runtime,mirror.initialApp(runtime),'open'),'back');assert.deepEqual(state,other);
 const blocked=appAction(runtime,original,'back');assert.equal(blocked,original);assert.equal(blocked.pointerEpoch,undefined);
});
test('native navigation rejects malformed model/history atomically and never resets it',()=>{
 const runtime={navigation:NAVIGATION_ENGINE,pages:[{id:'home'},{id:'detail'}]};
 const badStates=[{page:'missing',history:[]},{page:'home',history:Array(17).fill('home')},
  {page:'home',history:['missing']},{page:'home',history:new Array(1)},{page:'home',history:'home'}];
 for(const state of badStates){const before=structuredClone(state);
  assert.equal(navigationAllowed(runtime,state,{page:{reset:'home'}}),false);
  assert.equal(navigate(runtime,state,{page:{reset:'home'}}),false);assert.deepEqual(state,before);
 }
 for(const page of [{back:false},{push:'missing'},{push:'home',back:true},[],{unknown:'home'}]){
  const state={page:'home',history:[]};assert.equal(navigate(runtime,state,{page}),false);assert.deepEqual(state,{page:'home',history:[]});
 }
});
test('native navigation reloads its whole snapshot between independent instances',()=>{
 const runtime={navigation:NAVIGATION_ENGINE,pages:[{id:'home'},{id:'detail'},{id:'settings'}]};
 const first={page:'home',history:[]},second={page:'settings',history:['detail']};
 assert.equal(navigate(runtime,first,{page:{push:'detail'}}),true);
 assert.equal(navigate(runtime,second,{page:{back:true}}),true);
 assert.equal(navigate(runtime,first,{page:{back:true}}),true);
 assert.deepEqual(first,{page:'home',history:[]});assert.deepEqual(second,{page:'detail',history:[]});
});
