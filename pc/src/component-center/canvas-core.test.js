import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {validateCanvas,createCanvasCore,canvasCapabilityAccepts} from './canvas-core.js';
const fixture=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/canvas-v1.json',import.meta.url)));
const bytes=readFileSync(new URL('./canvas-core.wasm',import.meta.url));
test('canvas shared valid/rejection vectors and cost budgets',()=>{
  for(const c of fixture.valid)assert.doesNotThrow(()=>validateCanvas(c,fixture.runtime));
  for(const c of fixture.invalid)assert.throws(()=>validateCanvas(c,fixture.runtime));
  const c=structuredClone(fixture.valid[0]);c.w=320;c.commands=Array.from({length:7},()=>({op:'line',x:0,y:0,x2:1,y2:1,color:0}));
  assert.throws(()=>validateCanvas(c));c.commands.length=6;assert.doesNotThrow(()=>validateCanvas(c));
  c.commands=Array.from({length:17},()=>({op:'rect',x:0,y:0,w:1,h:1,color:1}));assert.throws(()=>validateCanvas(c));
});
test('canvas core has fixed memory and no ambient imports',()=>{
  const module=new WebAssembly.Module(bytes);assert.deepEqual(WebAssembly.Module.imports(module),[]);
  const instance=new WebAssembly.Instance(module,{});assert.equal(instance.exports.memory.buffer.byteLength,262144);
  assert.throws(()=>instance.exports.memory.grow(1));
});
test('canvas actual C/WASM emits only clipped rectangles and changes with typed variables',async()=>{
  const c=fixture.valid[0],core=await createCanvasCore(bytes,c,fixture.runtime);
  const first=core.project({x:12,y:8,w:16,h:6,r:5});
  assert.ok(first.length>0&&first.length<=2048);
  for(const [x,y,w,h,color]of first){assert.ok(x>=c.x&&y>=c.y&&w>0&&h>0&&x+w<=c.x+c.w&&y+h<=c.y+c.h&&color>=0&&color<=65535);}
  const next=core.project({x:2147483647,y:-2147483648,w:2147483647,h:0,r:2147483647});
  assert.notDeepEqual(next,first);assert.deepEqual(core.project({x:12,y:8,w:16,h:6,r:5}),first);
  for(const v of [null,'12',NaN,Infinity,2147483648])assert.throws(()=>core.project({x:v}));
  assert.throws(()=>core.project({}),{code:'canvas_snapshot_unavailable'});
  assert.deepEqual(core.project({x:12,y:8,w:16,h:6,r:5}),first,'failed snapshots never publish partial geometry');
});
test('canvas tiny and clipped circle retain exact pixel semantics',async()=>{
  const tiny=await createCanvasCore(bytes,fixture.valid[2]);assert.deepEqual(tiny.project({}),[[639,479,1,1,1]]);
  const circle=await createCanvasCore(bytes,fixture.valid[1]);
  const spans=circle.project({});assert.equal(spans.length,13);
  const pixels=new Set();for(const [x,y,w,h]of spans){assert.equal(h,1);for(let i=x;i<x+w;i++){assert.ok(i*i+y*y<=144);pixels.add(`${i},${y}`);}}
  for(let y=0;y<=12;y++)for(let x=0;x<=12;x++)assert.equal(pixels.has(`${x},${y}`),x*x+y*y<=144);
});
test('canvas all line octants terminate at the exact endpoint within fixed fuel',async()=>{
  for(const [sx,sy,tx,ty]of [[0,0,63,47],[63,47,0,0],[31,1,31,46],[1,20,62,20],[60,1,3,46],[3,46,60,1],[20,20,20,20]]){
    const core=await createCanvasCore(bytes,{engine:'p4-canvas-v1',x:0,y:0,w:64,h:48,commands:[{op:'line',x:sx,y:sy,x2:tx,y2:ty,color:1}]});
    const spans=core.project({});assert.equal(spans.length,Math.max(Math.abs(sx-tx),Math.abs(sy-ty))+1);
    assert.deepEqual(spans[0],[sx,sy,1,1,1]);assert.deepEqual(spans.at(-1),[tx,ty,1,1,1]);
    for(let i=1;i<spans.length;i++)assert.ok(Math.abs(spans[i][0]-spans[i-1][0])<=1&&Math.abs(spans[i][1]-spans[i-1][1])<=1);
  }
});
const fixtureV2=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/canvas-v2.json',import.meta.url)));
test('canvas v2 shared vectors, budgets and capability negotiation',()=>{
  for(const c of fixtureV2.valid)assert.doesNotThrow(()=>validateCanvas(c,fixtureV2.runtime),JSON.stringify(c).slice(0,80));
  for(const c of fixtureV2.invalid)assert.throws(()=>validateCanvas(c,fixtureV2.runtime),JSON.stringify(c).slice(0,80));
  assert.equal(canvasCapabilityAccepts('p4-canvas-v1','p4-canvas-v1'),true);assert.equal(canvasCapabilityAccepts('p4-canvas-v1','p4-canvas-v2'),true);
  assert.equal(canvasCapabilityAccepts('p4-canvas-v2','p4-canvas-v2'),true);assert.equal(canvasCapabilityAccepts('p4-canvas-v2','p4-canvas-v1'),false);
  assert.equal(canvasCapabilityAccepts('p4-canvas-v2',undefined),false);
});
test('canvas v2 WASM reproduces the reference raster byte for byte',async()=>{
  for(const vector of fixtureV2.raster){
    const core=await createCanvasCore(bytes,vector.declaration,fixtureV2.runtime),spans=core.project(vector.values||{});
    let hash=2166136261;for(const span of spans)for(const n of span)hash=Math.imul(hash^n,16777619)>>>0;
    assert.equal(spans.length,vector.count,vector.name);assert.equal(hash,vector.fnv,vector.name);
    const c=vector.declaration;for(const [x,y,w,h,color]of spans)assert.ok(x>=c.x&&y>=c.y&&w>0&&h>0&&x+w<=c.x+c.w&&y+h<=c.y+c.h&&color>=0&&color<=65535);
    assert.ok(spans.length<=(c.engine==='p4-canvas-v2'?8192:2048));
  }
  const ring=await createCanvasCore(bytes,{engine:'p4-canvas-v2',x:0,y:0,w:32,h:32,commands:[{op:'ring',x:16,y:16,r:10,inner:6,color:9}]});
  const covered=new Set();for(const [x,y,w,h]of ring.project({})){assert.equal(h,1);for(let i=x;i<x+w;i++)covered.add(`${i},${y}`);}
  for(let y=0;y<32;y++)for(let x=0;x<32;x++){const d=(x-16)**2+(y-16)**2;assert.equal(covered.has(`${x},${y}`),d<=100&&!(Math.abs(y-16)<=6&&(x-16)**2<=36-(y-16)**2),`${x},${y}`);}
});
test('particle preview core reproduces the shared simulation traces',async()=>{
  const {createParticleCore}=await import('./canvas-core.js');
  const fixture=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/particles-v1.json',import.meta.url)));
  for(const vector of fixture.simulation){
    const core=await createParticleCore(bytes);core.begin(0,vector.config,vector.seed);const out=[];
    for(const ev of vector.events){if(ev.burst)core.burst(0);const alive=core.step(0,ev.now,ev.emitting!==false);const dots=core.dots(0);let hash=2166136261;for(const d of dots)for(const n of d)hash=Math.imul(hash^n,16777619)>>>0;out.push([alive,dots.length,hash]);
      const c=vector.config;for(const [x,y,size]of dots)assert.ok(x>=c.x&&x<c.x+c.w&&y>=c.y&&y<c.y+c.h&&(size===1||size===3));}
    assert.deepEqual(out,vector.trace,vector.name);
  }
  const core=await createParticleCore(bytes);assert.throws(()=>core.begin(1,{...fixture.simulation[0].config,count:65},1));
});
