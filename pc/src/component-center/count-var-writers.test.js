import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {validateClawpkgManifest} from '../lib/clawpkg-contract.js';
import * as preview from './app-runtime.js';
import * as mirrored from '../../skills/petui/scripts/app-runtime.mjs';
import {dSnapshot} from './d-surface.js';
import {dSnapshot as mirrorSnapshot} from '../../skills/petui/scripts/d-surface.mjs';
const suite=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/count-var-writers-v1.json',import.meta.url)));
function runtime(c){const r=structuredClone(suite.bases[c.base]);for(const p of c.patches){let at=r;for(const k of p.path.slice(0,-1))at=at[k];at[p.path.at(-1)]=structuredClone(p.value);}return r;}
for(const c of suite.cases)test(`count writer complete manifest: ${c.name}`,()=>{
 const r=runtime(c),manifest={...structuredClone(suite.packages[c.base]),'runtime/widget.json':r,'assets/':[],'runtime/':[]};
 const admission=validateClawpkgManifest(manifest);assert.equal(admission.valid,c.ok,admission.errors.join('\n'));
 for(const [model,project] of [[preview,dSnapshot],[mirrored,mirrorSnapshot]]){
  if(!c.ok){assert.ok(admission.errors.some(e=>e.includes('guest set/inc target')));assert.throws(()=>model.initialApp(r),/guest set\/inc target/);assert.throws(()=>project(r,{vars:{page:0,pages:99,probe:0}}),/guest set\/inc target/);continue;}
  const state=model.initialApp(r),before=structuredClone(state),shown=project(r,state);assert.deepEqual(state,before);
  if(c.expectedProbe!==undefined){
   const next=['main','else'].includes(c.branch)?model.appAction(r,shown,'next'):model.appAdvance(r,state,100);
   const saved=structuredClone(next),rendered=project(r,next);assert.equal(next.vars.probe,c.expectedProbe);assert.equal(rendered.vars.probe,c.expectedProbe);assert.equal(rendered.vars.pages,4);assert.deepEqual(next,saved);
  }
  if(c.base==='unowned'){const next=['main','else'].includes(c.branch)?model.appAction(r,shown,'next'):model.appAdvance(r,state,100);assert.equal(next.vars.pages,c.op==='set'?99:2);}
 }
});
test('read-only sources remain observable through action and timer snapshots without changing count ownership',()=>{
 const c=suite.cases.find(c=>c.name==='allow-read-sources-conditions-persist-limits'),r=runtime(c);
 for(const [model,project] of [[preview,dSnapshot],[mirrored,mirrorSnapshot]]){
  const raw=model.initialApp(r),shown=project(r,raw),action=model.appAction(r,shown,'next');assert.equal(action.vars.probe,4);assert.equal(action.vars.pages,4);
  const timer=model.appAdvance(r,raw,100),saved=structuredClone(timer),rendered=project(r,timer);assert.equal(timer.vars.probe,8);assert.equal(timer.vars.pages,4);assert.equal(rendered.vars.pages,4);assert.deepEqual(timer,saved);
 }
});
test('cold derived count is stored before ten read-only timers and rendering never mutates snapshots',()=>{
 const r=structuredClone(suite.bases.reader);r.tick=[{every_ms:100,set:{probe:{var:'pages'}}}];
 assert.equal(r.vars.pages.init,1);const declaration=structuredClone(r);
 for(const [model,project] of [[preview,dSnapshot],[mirrored,mirrorSnapshot]]){
  let stored=model.initialApp(r);const initial=structuredClone(stored),firstRender=project(r,stored);
  assert.equal(stored.vars.pages,4);assert.equal(stored.vars.probe,0);assert.equal(firstRender.vars.pages,4);assert.deepEqual(stored,initial);
  for(let i=1;i<=10;i++){
   const previous=stored,before=structuredClone(previous);stored=model.appAdvance(r,previous,100);assert.deepEqual(previous,before);
   const saved=structuredClone(stored),rendered=project(r,stored);
   assert.equal(stored.clock,i*100);assert.equal(stored.vars.pages,4);assert.equal(stored.vars.probe,4);assert.equal(rendered.vars.pages,4);assert.equal(rendered.vars.probe,4);assert.deepEqual(stored,saved);
  }
 }
 assert.deepEqual(r,declaration);
});
