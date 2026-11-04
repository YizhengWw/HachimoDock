import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createParticleCore} from './canvas-core.js';
import {particleGuard,stepParticlePreview} from './particle-preview.js';
const fixture=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/particles-v1.json',import.meta.url)));
const bytes=readFileSync(new URL('./canvas-core.wasm',import.meta.url));
test('guards require typed snapshots and known operators',()=>{
  const e={when:{var:'n',op:'gte',value:2}};
  for(const vars of [undefined,{}, {n:'2'}, {n:NaN}, Object.create({n:3})])assert.equal(particleGuard(e,vars),false);
  assert.equal(particleGuard(e,{n:2}),true);
  assert.equal(particleGuard({...e,when:{...e.when,op:'unknown'}},{n:2}),false);
});
test('page filtering, guard changes, action saturation and overlay slots follow renderer order',()=>{
  const calls=[],core={burst:i=>calls.push(['burst',i]),step:(...args)=>calls.push(['step',...args]),dots:()=>[[1,2,1,65535]]};
  const emitters=[{page:1,trigger:{action:'go'}},{page:4,trigger:{action:'go'},when:{var:'n',op:'eq',value:1}}];
  assert.deepEqual(stepParticlePreview(core,emitters,{page:0,clock:20,vars:{n:0},actions:['go']}),[[1,2,1,65535,1]]);
  assert.deepEqual(calls,[['step',1,20,false]]);calls.length=0;
  stepParticlePreview(core,emitters,{page:1,clock:40,vars:{n:1},actions:Array(9).fill('go')});
  assert.equal(calls.filter(c=>c[0]==='burst').length,8);
  assert.deepEqual(calls.filter(c=>c[0]==='step'),[['step',0,40,true],['step',1,40,true]]);
  assert.throws(()=>stepParticlePreview(core,emitters,{clock:NaN}));
});
test('real WASM particle overlay remains bounded and ages when its guard closes',async()=>{
  const e=structuredClone(fixture.valid[0].particles[0]);
  e.trigger={action:'go'};delete e.page;delete e.when;
  const core=await createParticleCore(bytes);core.begin(0,e,17);
  let dots=stepParticlePreview(core,[e],{clock:0,actions:['go']});
  assert.ok(dots.length>0&&dots.length<=e.budget);
  for(const [x,y]of dots)assert.ok(x>=e.x&&x<e.x+e.w&&y>=e.y&&y<e.y+e.h);
  e.when={var:'enabled',op:'eq',value:1};
  for(let clock=20;clock<=e.life_ms+200;clock+=20)dots=stepParticlePreview(core,[e],{clock,vars:{enabled:0},actions:['go']});
  assert.deepEqual(dots,[]);
});
