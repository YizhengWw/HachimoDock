import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {PROGRAM_ENGINE,encodeProgram,inspectProgram,createProgramPreview} from './program-core.js';
import * as mirror from '../../skills/petui/scripts/program-core.mjs';
const fixture=JSON.parse(fs.readFileSync(new URL('../../skills/petui/contracts/program-preview-v1.json',import.meta.url)));
const declaration=c=>({engine:PROGRAM_ENGINE,schema:37,cells:c.cells,fuel:512,code:c.code});
const seed=c=>({schema:37,revision:0,cells:c.cells,values:c.initial,outputs:Array(8).fill(0)});
function finish(p,maximum=4100){
 for(let i=0;i<maximum;i++){const r=p.pump();if(r==='complete')return p.takeReport();assert.equal(r,'yielded');}
 assert.fail('bounded preview failed to terminate');
}
for(const c of fixture.cases)for(const quantum of [1,7,64])test(`shared program ${c.name}, quantum ${quantum}`,()=>{
 const bytes=encodeProgram(declaration(c)),p=createProgramPreview(bytes,{seed:seed(c),quantum}),other=mirror.createProgramPreview(bytes,{seed:seed(c),quantum});
 assert.deepEqual(inspectProgram(bytes),{engine:PROGRAM_ENGINE,schema:37,cells:c.cells,fuel:512,instructions:c.code.length});
 for(const [i,event]of c.events.entries()){
  const before=p.snapshot();assert.equal(p.enqueue(i+1,event),'ok');assert.equal(other.enqueue(i+1,event),'ok');
  assert.equal(p.pump(),'yielded');assert.equal(other.pump(),'yielded');assert.deepEqual(p.snapshot(),before); /* begin is private */
  const a=finish(p),b=finish(other);assert.deepEqual(a,b);assert.equal(a.result,'committed');assert.equal(a.sequence,i+1);assert.equal(a.revision,i+1);assert(a.usedFuel<=512);
 }
 const out=p.snapshot();assert.deepEqual(out,other.snapshot());assert.deepEqual(out.values,c.values);assert.deepEqual(out.outputs,[c.output,...Array(7).fill(0)]);assert.equal(out.revision,c.events.length);
});
test('program input schemas and native image checks reject effects and corruption',()=>{
 const c=fixture.cases[0],p=declaration(c);
 for(const mutation of [q=>q.extra=true,q=>q.cells=65,q=>q.fuel=0,q=>q.schema=0,q=>q.code=[['CONST',16,0,0,0]],q=>q.code=[['HALT',0,0,0,1]],q=>q.code=[['JUMP',0,0,0,-1]],q=>q.code=[['INTENT',0,0,0,0]],q=>q.code[0][4]=true]){
  const q=structuredClone(p);mutation(q);assert.throws(()=>encodeProgram(q));
 }
 const bytes=encodeProgram(p);for(const offset of [0,4,5,6,8,12,16,20,24,bytes.length-1]){const q=bytes.slice();q[offset]^=128;assert.throws(()=>createProgramPreview(q));}
 assert.throws(()=>inspectProgram(new Uint8Array(2073)));assert.throws(()=>inspectProgram([...bytes]));
 assert.throws(()=>encodeProgram({...p,code:Array(2)}));
 const wasm=fs.readFileSync(new URL('./program-core.wasm',import.meta.url)),m=new WebAssembly.Module(wasm);assert.deepEqual(WebAssembly.Module.imports(m),[]);
 const e=new WebAssembly.Instance(m,{}).exports;assert.equal(e.memory.buffer.byteLength,262144);assert.throws(()=>e.memory.grow(1),RangeError);
 new Uint8Array(e.memory.buffer,e.pp_buffer(),bytes.length).set(bytes);assert.equal(e.pp_configure(bytes.length),0);
 for(const q of [0,65,65537])assert.equal(e.pp_start(q,0),1); /* cannot truncate into a valid uint16 quantum */
 e.pp_seed(37,0,256);assert.equal(e.pp_start(1,1),6); /* no narrowing or implicit blank restore */
 assert.equal(e.pp_start(1,2),1);assert.equal(e.pp_start(1,0),0);assert.equal(e.pp_start(1,0),1);
});
test('program traps roll back partial writes; fuel exhaustion is terminal, not an automatic retry',()=>{
 const p=createProgramPreview(encodeProgram({engine:PROGRAM_ENGINE,schema:1,cells:1,fuel:10,code:[['CONST',0,0,0,0],['CONST',1,0,0,9],['STORE',0,1,0,0],['JUMP',0,0,0,3]]}),{quantum:1});
 const before=p.snapshot();assert.equal(p.enqueue(1,[]),'ok');const report=finish(p);assert.equal(report.result,'fuel_exhausted');assert.equal(report.usedFuel,10);assert.deepEqual(p.snapshot(),before);assert.equal(p.enqueue(1,[]),'duplicate');assert.equal(p.pump(),'idle');
});
test('program queue and unread results enforce backpressure; cancellation never restarts or relabels a completed event',()=>{
 const bytes=encodeProgram({engine:PROGRAM_ENGINE,schema:1,cells:0,fuel:1,code:[['HALT',0,0,0,0]]}),p=createProgramPreview(bytes);
 for(let i=1;i<=4;i++)assert.equal(p.enqueue(i,[]),'ok');assert.equal(p.enqueue(5,[]),'queue_full');
 assert.equal(p.pump(),'yielded');assert.equal(p.pump(),'yielded');assert.equal(p.pump(),'complete');assert.equal(p.pump(),'report_pending');
 assert.equal(p.cancel(),'ok');const r=p.takeReport();assert.equal(r.sequence,1);assert.equal(r.result,'committed');assert.equal(p.pump(),'stopped');assert.equal(p.enqueue(5,[]),'stopped');assert.equal(p.snapshot().revision,1);
});
test('program restore is exact, never implicit migration/reset; sequence handles retain safe-integer precision',()=>{
 const bytes=encodeProgram({engine:PROGRAM_ENGINE,schema:4294967295,cells:1,fuel:1,code:[['HALT',0,0,0,0]]});
 const s={schema:4294967295,revision:4294967295,cells:1,values:[-2147483648],outputs:[2147483647,...Array(7).fill(0)]};
 const p=createProgramPreview(bytes,{seed:s});assert.deepEqual(p.snapshot(),s);
 assert.equal(p.enqueue(Number.MAX_SAFE_INTEGER,[]),'ok');const r=finish(p);assert.equal(r.sequence,Number.MAX_SAFE_INTEGER);assert.equal(r.result,'overflow');assert.deepEqual(p.snapshot(),s);
 for(const q of [{...s,schema:1},{...s,values:[]},{...s,outputs:[]},{...s,outputs:Array(8)},{...s,values:Array(1)},{...s,cells:0},{...s,revision:-1},{...s,values:[2147483648]},{...s,extra:1}])assert.throws(()=>createProgramPreview(bytes,{seed:q}));
 for(const event of [[true],[0,0,0,0,0],[NaN],[2147483648],Array(1)])assert.throws(()=>p.enqueue(2,event));
 for(const sequence of [0,-1,Number.MAX_SAFE_INTEGER+1,NaN])assert.throws(()=>p.enqueue(sequence,[]));
 for(const quantum of [0,65,NaN,true])assert.throws(()=>createProgramPreview(bytes,{quantum}));
});
test('preview instances and input buffers cannot share or revive each other’s authority',()=>{
 const bytes=encodeProgram(declaration(fixture.cases[0])),a=createProgramPreview(bytes),b=createProgramPreview(bytes);
 bytes.fill(0);assert.equal(a.enqueue(1,[0,9]),'ok');assert.equal(b.enqueue(1,[0,3]),'ok');assert.equal(a.cancel(),'ok');assert.equal(a.pump(),'stopped');assert.equal(finish(b).result,'committed');assert.equal(b.snapshot().values[0],3);assert.equal(a.snapshot().values[0],0);
 assert.equal(fs.readFileSync(new URL('./program-core.js',import.meta.url),'utf8'),fs.readFileSync(new URL('../../skills/petui/scripts/program-core.mjs',import.meta.url),'utf8'));
});
const outputLimits=()=>Array.from({length:8},()=>({min:-2147483648,max:2147483647}));
const eventOutput=(port=0)=>encodeProgram({engine:PROGRAM_ENGINE,schema:9,cells:1,fuel:32,code:[['CONST',0,0,0,0],['EVENT',1,0,0,0],['STORE',0,1,0,0],['OUTPUT',port,1,0,0],['HALT',0,0,0,0]]});
test('receiver limits are copied and a violating output rolls back cells, outputs and revision together',()=>{
 const limits=outputLimits();limits[0]={min:-10,max:10};
 const p=createProgramPreview(eventOutput(),{quantum:1,outputLimits:limits}),before=p.snapshot();limits[0].max=1000;
 assert.equal(p.enqueue(1,[11]),'ok');const rejected=finish(p);assert.equal(rejected.result,'bounds');assert.equal(rejected.revision,0);assert.deepEqual(p.snapshot(),before);
 assert.equal(p.enqueue(1,[11]),'duplicate');assert.equal(p.pump(),'idle');
 assert.equal(p.enqueue(2,[10]),'ok');assert.equal(finish(p).result,'committed');assert.equal(p.snapshot().values[0],10);assert.equal(p.snapshot().outputs[0],10);assert.equal(p.snapshot().revision,1);
});
test('every output port enforces inclusive int32 ranges, not just port zero',()=>{
 for(let port=0;port<8;port++){
  const limits=outputLimits();limits[port]={min:-2,max:2};const p=createProgramPreview(eventOutput(port),{outputLimits:limits});
  for(const [i,value]of [-2,2,-3,3].entries()){
   const before=p.snapshot();assert.equal(p.enqueue(i+1,[value]),'ok');const r=finish(p);assert.equal(r.result,i<2?'committed':'bounds');
   if(i<2)assert.equal(p.snapshot().outputs[port],value);else assert.deepEqual(p.snapshot(),before);
  }
 }
});
test('receiver limits never reset or clamp an incompatible restored state, nor invent initial outputs',()=>{
 const bytes=eventOutput(),seed={schema:9,revision:12,cells:1,values:[25],outputs:[10,...Array(7).fill(0)]},copy=structuredClone(seed),limits=outputLimits();limits[0]={min:-9,max:9};
 assert.throws(()=>createProgramPreview(bytes,{seed,outputLimits:limits}));assert.deepEqual(seed,copy);
 limits[0].max=10;assert.deepEqual(createProgramPreview(bytes,{seed,outputLimits:limits}).snapshot(),seed);
 limits[0]={min:1,max:10};assert.throws(()=>createProgramPreview(bytes,{outputLimits:limits}));
 assert.deepEqual(createProgramPreview(bytes,{seed,outputLimits:limits}).snapshot(),seed);
 const legacy=createProgramPreview(bytes);assert.equal(legacy.enqueue(1,[2147483647]),'ok');assert.equal(finish(legacy).result,'committed');assert.equal(legacy.snapshot().outputs[0],2147483647);
});
test('malformed receiver contracts fail closed; raw ABI cannot admit partial or poisoned limits',()=>{
 const bytes=eventOutput();
 for(const bad of [[],Array(8),outputLimits().slice(1),[...outputLimits(),{min:0,max:1}]])assert.throws(()=>createProgramPreview(bytes,{outputLimits:bad}));
 for(const change of [q=>q.min=1,q=>q.min=true,q=>q.max=2147483648,q=>q.extra=0,q=>delete q.min]){
  const limits=outputLimits();limits[0]={min:0,max:0};change(limits[0]);assert.throws(()=>createProgramPreview(bytes,{outputLimits:limits}));
 }
 const module=new WebAssembly.Module(fs.readFileSync(new URL('./program-core.wasm',import.meta.url)));
 const fresh=()=>{const e=new WebAssembly.Instance(module,{}).exports;new Uint8Array(e.memory.buffer,e.pp_buffer(),bytes.length).set(bytes);assert.equal(e.pp_configure(bytes.length),0);return e;};
 let e=fresh();assert.equal(e.pp_limit(0,0,1),1);assert.equal(e.pp_start(1,0),6);
 e=fresh();assert.equal(e.pp_limit(8,0,1),0);for(let i=0;i<8;i++)assert.equal(e.pp_limit(i,0,1),1);assert.equal(e.pp_start(1,0),6);
 e=fresh();for(let i=0;i<8;i++)assert.equal(e.pp_limit(i,0,1),1);assert.equal(e.pp_start(1,0),0);assert.equal(e.pp_limit(0,0,100),0);
});
test('shared explicit timer clock enqueues one fair event per tick, coalesces lateness and never pumps a program',()=>{
 const clock={nowMs:100,timers:[{id:1,delayMs:0,intervalMs:100,words:[5]},{id:2,delayMs:0,intervalMs:100,words:[7]}]};
 const p=createProgramPreview(eventOutput(),{quantum:1,clock});clock.timers[0].words[0]=99;
 assert.deepEqual(p.tick(1100,1),{status:'enqueued',timerId:1,coalesced:10,accepted:true,deadlineExhausted:false});
 assert.equal(p.snapshot().revision,0);assert.equal(p.tick(1100,2).timerId,2);assert.equal(p.tick(1100,3).status,'idle');
 assert.equal(finish(p).result,'committed');assert.equal(p.snapshot().outputs[0],5);
 assert.equal(finish(p).result,'committed');assert.equal(p.snapshot().outputs[0],7);
 assert.equal(p.tick(1200,3).timerId,1);assert.equal(p.cancel(),'ok');assert.equal(p.tick(1300,4).status,'stopped');assert.equal(p.pump(),'stopped');
});
test('timer backpressure preserves a due event; clock regressions cancel without resetting committed state',()=>{
 const p=createProgramPreview(eventOutput(),{clock:{nowMs:0,timers:[{id:1,delayMs:0,intervalMs:100,words:[8]}]}});
 for(let i=1;i<=4;i++)assert.equal(p.enqueue(i,[i]),'ok');
 assert.equal(p.tick(2000,5).status,'backpressure');assert.equal(p.tick(2000,5).accepted,false);
 assert.equal(finish(p).result,'committed');assert.equal(p.tick(2100,5).status,'enqueued');
 assert.equal(p.tick(2099,6).status,'clock_error');assert.equal(p.pump(),'stopped');assert.equal(p.snapshot().outputs[0],1);assert.equal(p.tick(2200,6).status,'stopped');
 const q=createProgramPreview(eventOutput(),{clock:{nowMs:0,timers:[{id:1,delayMs:0,intervalMs:100,words:[8]}]}});
 assert.equal(q.enqueue(1,[3]),'ok');assert.equal(q.tick(0,1).status,'sequence_error');assert.equal(q.pump(),'stopped');assert.equal(q.snapshot().revision,0);
});
test('timer schedules require explicit clock data, bounded distinct specifications and valid intervals',()=>{
 const bytes=eventOutput(),good={nowMs:0,timers:[{id:1,delayMs:0,intervalMs:100,words:[3]}]};
 for(const change of [c=>c.nowMs=-1,c=>c.nowMs=Number.MAX_SAFE_INTEGER+1,c=>c.extra=1,c=>c.timers=[],c=>c.timers=Array(1),c=>c.timers=[c.timers[0],c.timers[0]],c=>c.timers[0].id=0,c=>c.timers[0].intervalMs=99,c=>c.timers[0].delayMs=4294967296,c=>c.timers[0].words=[true],c=>c.timers[0].words=Array(1),c=>c.timers[0].extra=1]){
  const c=structuredClone(good);change(c);assert.throws(()=>createProgramPreview(bytes,{clock:c}));
 }
 const p=createProgramPreview(bytes);assert.equal(p.tick(0,1).status,'invalid');
 for(const [now,seq]of [[-1,1],[0,0],[NaN,1],[0,Number.MAX_SAFE_INTEGER+1]])assert.throws(()=>p.tick(now,seq));
 const one=createProgramPreview(bytes,{clock:{nowMs:2**40,timers:[{id:7,delayMs:10,intervalMs:0,words:[1]}]}});
 assert.equal(one.tick(2**40+9,1).status,'idle');assert.equal(one.tick(2**40+10,1).timerId,7);assert.equal(one.tick(Number.MAX_SAFE_INTEGER,2).status,'idle');
});
