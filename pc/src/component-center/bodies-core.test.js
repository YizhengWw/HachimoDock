import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {validateClawpkgManifest} from '../lib/clawpkg-contract.js';
import {createBodiesPreview,validateBodiesScene} from './bodies-core.js';
const bytes=readFileSync(new URL('./classic-core.wasm',import.meta.url));
const f=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/bodies-v1.json',import.meta.url)));
test('shared bodies schema valid/rejection vectors',()=>{
  for(const s of f.valid)assert.doesNotThrow(()=>validateBodiesScene(s));
  for(const s of f.invalid)assert.throws(()=>validateBodiesScene(s),JSON.stringify(s));
});
test('same device core exchanges velocity, fires collision rules and resets rolling state',async()=>{
  const core=await createBodiesPreview(bytes,f.valid[0]);core.tick(1);for(let t=21;t<=201;t+=20)core.tick(t);
  const [a,b]=core.snapshot().entities;assert.equal(a.vx,-4);assert.equal(b.vx,4);assert.ok(a.x+a.width<=b.x);assert.notEqual(a.rotation,0);assert.equal(core.snapshot().score,1);
  core.action('start',202);assert.equal(core.snapshot().score,0);assert.equal(core.snapshot().entities[0].x,2);assert.equal(core.snapshot().entities[0].rotation,0);
});
test('gravity, impulse, static bodies, catch-up, pause and deterministic replay use C only',async()=>{
  const s=structuredClone(f.valid[0]);s.entities[1].vx=0;s.bodies[1].static=true;s.gravity=32;
  const a=await createBodiesPreview(bytes,s),b=await createBodiesPreview(bytes,s);
  for(const core of [a,b]){core.tick(1);core.action('kick',2);core.tick(161);}
  assert.deepEqual(a.snapshot(),b.snapshot());assert.equal(a.snapshot().entities[1].x,4);assert.equal(a.snapshot().entities[1].y,2);
  const before=a.snapshot();a.hold(100000);a.tick(100001);assert.deepEqual(a.snapshot(),before);
  a.tick(100021);assert.notDeepEqual(a.snapshot(),before);
  b.tick(100000);const after=b.snapshot();b.tick(100000);assert.deepEqual(b.snapshot(),after);
});
test('WASM is isolated with fixed memory and owns its snapshot data',async()=>{
  const module=new WebAssembly.Module(bytes);assert.deepEqual(WebAssembly.Module.imports(module),[]);
  const core=await createBodiesPreview(bytes,f.valid[0]);const frame=core.snapshot();frame.entities[0].x=1000;assert.equal(core.snapshot().entities[0].x,2);
  const e=new WebAssembly.Instance(module,{}).exports;assert.equal(e.memory.buffer.byteLength,262144);assert.throws(()=>e.memory.grow(1));
});

test('mass and restitution obey analytical one-dimensional collisions',async()=>{
  for(const [elasticity,expected] of [[100,[-8,0]],[0,[-2,-2]]]){
    const s=structuredClone(f.valid[0]);s.bodies[1].mass=3;s.bodies.forEach(b=>b.restitution=elasticity);
    const core=await createBodiesPreview(bytes,s);core.tick(1);for(let t=21;t<=201;t+=20)core.tick(t);
    assert.deepEqual(core.snapshot().entities.map(e=>e.vx),expected);
  }
});
test('high-speed impulse cannot tunnel through a one-cell static body at maximum tick',async()=>{
  const s=structuredClone(f.valid[0]);s.tick_ms=200;s.entities[0].x=0;s.entities[0].vx=0;s.entities[1].x=8;s.entities[1].vx=0;s.bodies[1].static=true;
  s.rules[1].do[0]={op:'impulse',entity:'a',dx:64,dy:0};
  const core=await createBodiesPreview(bytes,s);core.tick(1);core.action('kick',1);core.tick(201);
  const [a,b]=core.snapshot().entities;assert.equal(b.x,8);assert.ok(a.x+a.width<=b.x);assert.ok(a.vx<0);assert.ok(core.snapshot().score>=1);
});
test('scene stop and hidden/non-body entities preserve the shared lifecycle',async()=>{
  const s=structuredClone(f.valid[0]);s.entities.push({id:'decor',x:10,y:10,active:false});
  s.rules.push({on:'pause',do:[{op:'stop'}]},{on:'hide',do:[{op:'hide',entity:'a'}]});
  const core=await createBodiesPreview(bytes,s);core.tick(1);core.action('pause',2);const held=core.snapshot();core.tick(99999);assert.deepEqual(core.snapshot(),held);
  core.action('start',100000);core.action('hide',100001);assert.equal(core.snapshot().entities.some(e=>e.id==='a'),false);assert.equal(core.snapshot().entities.some(e=>e.id==='decor'),false);
});

test('frontend package admission accepts bodies and rejects the shared invalid vectors',()=>{
  const base={'component.json':{id:'body-demo',name:'Bodies',version:'1',kind:'game'},'negative-screen.json':{},'runtime/':{},'assets/':{},'share.json':{},'buttons.json':[{action:'start',control:'SW1',event:'button.sw1.short_press',label:'Start'},{action:'kick',control:'SW2',event:'button.sw2.short_press',label:'Kick'}]};
  for(const scene of f.valid){const result=validateClawpkgManifest({...base,'runtime/widget.json':{...f.runtime,scene}});assert.equal(result.valid,true,result.errors.join(' '));}
  for(const scene of f.invalid)assert.equal(validateClawpkgManifest({...base,'runtime/widget.json':{...f.runtime,scene}}).valid,false);
});
