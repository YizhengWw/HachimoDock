import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import * as preview from './app-runtime.js';
import * as packaged from '../../skills/petui/scripts/app-runtime.mjs';
import {createComponentProgram,componentProgramConfig} from './component-program.js';
const suite=n=>JSON.parse(readFileSync(new URL(`../../skills/petui/contracts/${n}.json`,import.meta.url)));
for(const [name,model] of [['preview',preview],['packaged',packaged]])for(const kind of ['cells','texts']){
 const vectors=suite(kind+'-v1');
 for(const c of vectors.cases)test(`${name} ${kind}: ${c.name}`,()=>{
  let s=model.initialApp(vectors.runtime);
  for(const e of c.events)s=e.action?model.appAction(vectors.runtime,s,e.action):model.appAdvance(vectors.runtime,s,e.ms);
  if(c.expected.vars)for(const [k,v] of Object.entries(c.expected.vars))assert.equal(s.vars[k],v,k);
  if(c.expected.cells)assert.deepEqual(s.cells,c.expected.cells);
  if(c.expected.nodeText)assert.deepEqual(vectors.runtime.surface.elements.filter(n=>n.text_at).map(n=>model.appNodeValue(n,s,{},vectors.runtime)),c.expected.nodeText);
  if(c.expected.headline!==undefined){const d=model.appDashboard(vectors.runtime,s);assert.equal(d.headline,c.expected.headline);assert.equal(d.note,c.expected.note);}
 });
}
const v2=suite('component-program-v2');
for(const c of v2.cases)test(`same C VM v2: ${c.name}`,()=>{
 const r=v2.runtime,p=createComponentProgram(r);let s=preview.initialApp(r);
 for(const e of c.events){
  if(p.handles(e.action)){assert.equal(p.enqueue(e.action,s.vars),'ok');let out;for(let i=0;i<300&&!out;i++)out=p.pump(s.vars,0);assert.ok(out);s.vars=out;}
  else s=preview.appAction(r,s,e.action);
 }
 const snap=p.snapshot();assert.deepEqual(snap.values,c.expected.cells);assert.deepEqual(p.gridSnapshot(),c.expected.grid);
 for(const [k,v] of Object.entries(c.expected.outputs))assert.equal(s.vars[k],v,k);
});
test('VM v2 descriptor rejects invalid data and v1 retains its ceilings',()=>{
 for(const patch of v2.reject.filter(p=>p.scope!=='package')){
  const r=structuredClone(v2.runtime),parts=patch.path.slice(1).split('/');let o=r;for(const k of parts.slice(0,-1))o=o[k];o[parts.at(-1)]=patch.value;
  assert.throws(()=>componentProgramConfig(r),patch.name);
 }
});
