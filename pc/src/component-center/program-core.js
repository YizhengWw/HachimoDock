/** Offline authoring/preview only. One fixed C interpreter, no package JS/WASM.
 * Not a device grant, package loader, effect engine or wall-time guarantee.
 * The caller schedules ONE pump per UI turn; no automatic run/retry/resume. */
import {encodedProgramCoreV2} from './program-bytes-v2.mjs';
import {encodedProgramCore} from './program-bytes.mjs';
export const PROGRAM_ENGINE='p4-integer-program-v1';
export const PROGRAM_OPS=Object.freeze(['HALT','CONST','MOVE','ADD','SUB','MUL','DIV','MOD','LT','EQ','JUMP','JZ','LOAD','STORE','EVENT','OUTPUT']);
const RUNTIME=Object.freeze(['ok','invalid','busy','stale','denied','bad_image','bad_state','queue_full','duplicate','idle','yielded','complete','report_pending','stopped']);
const RESULT=Object.freeze(['empty','ready','committed','invalid','bad_pc','fuel_exhausted','div_zero','overflow','bounds','cancelled','capability','call_limit','conflict','outbox_busy','not_ready','running']);
const TIMER=Object.freeze(['ok','invalid','busy','stale','idle','enqueued','backpressure','stopped','clock_error','sequence_error']);
const integer=(n,lo,hi)=>Number.isInteger(n)&&n>=lo&&n<=hi;
const i32=n=>integer(n,-2147483648,2147483647),u32=n=>integer(n,0,4294967295);
const dense=(a,minimum,maximum)=>Array.isArray(a)&&a.length>=minimum&&a.length<=maximum&&Object.keys(a).length===a.length&&Array.from({length:a.length},(_,i)=>Object.hasOwn(a,i)).every(Boolean);
const exact=(o,keys)=>o&&typeof o==='object'&&!Array.isArray(o)&&Object.keys(o).length===keys.length&&keys.every(k=>Object.hasOwn(o,k));
const fail=()=>{throw Error('Invalid bounded integer program');};
let compiled,compiledV2;
function freshCore(bytes,{grid=null,texts=0}={}){
 const v2=bytes instanceof Uint8Array&&bytes[4]===2;
 if(!(bytes instanceof Uint8Array)||bytes.length<24||bytes.length>(v2?8216:2072))fail();
 let module=v2?compiledV2:compiled;
 if(!module){module=new WebAssembly.Module(Uint8Array.from(atob(v2?encodedProgramCoreV2:encodedProgramCore),c=>c.charCodeAt(0)));if(WebAssembly.Module.imports(module).length)fail();if(v2)compiledV2=module;else compiled=module;}
 const e=new WebAssembly.Instance(module,{}).exports;
 if(e.memory.buffer.byteLength!==262144)fail();
 new Uint8Array(e.memory.buffer,e.pp_buffer(),bytes.length).set(bytes);
 if(e.pp_configure(bytes.length)!==0)fail();
 if(v2){if(grid!==null&&(!exact(grid,["cols","rows"])||!e.pp_grid(grid.cols,grid.rows)))fail();if(!integer(texts,0,64)||!e.pp_texts(texts))fail();}
 return e;
}
export function inspectProgram(bytes){
 const e=freshCore(bytes);return Object.freeze({engine:bytes[4]===2?"p4-integer-program-v2":PROGRAM_ENGINE,schema:e.pp_schema()>>>0,cells:e.pp_cells(),fuel:e.pp_fuel(),instructions:e.pp_count()});
}
/* This assembles DATA instructions only. Native image validation remains the
 * authority for register/jump operands, reserved bits and forbidden effects. */
export function encodeProgram(p){
 const v2=p?.engine==="p4-integer-program-v2",ops=v2?[...PROGRAM_OPS,"TEXT","CELLGET","CELLSET"]:PROGRAM_OPS;
 if(!exact(p,['engine','schema','cells','fuel','code'])||(!v2&&p.engine!==PROGRAM_ENGINE)||!integer(p.schema,1,4294967295)||!integer(p.cells,0,v2?256:64)||!integer(p.fuel,1,v2?16384:4096)||!dense(p.code,1,v2?1024:256))fail();
 const bytes=new Uint8Array(24+8*p.code.length),v=new DataView(bytes.buffer);
 bytes.set([80,52,80,49,v2?2:1,0,24,0]);v.setUint16(8,p.code.length,true);v.setUint16(10,p.fuel,true);v.setUint16(12,p.cells,true);v.setUint32(16,p.schema,true);
 p.code.forEach((op,i)=>{
  if(!dense(op,5,5)||typeof op[0]!=='string'||!ops.includes(op[0])||!op.slice(1,4).every(n=>integer(n,0,255))||!i32(op[4]))fail();
  const offset=24+8*i;bytes.set([(ops.indexOf(op[0])>=16?ops.indexOf(op[0])+1:ops.indexOf(op[0])),...op.slice(1,4)],offset);v.setInt32(offset+4,op[4],true);
 });
 let crc=0xffffffff;
 for(let i=0;i<bytes.length;i++)if(i<20||i>=24){crc^=bytes[i];for(let j=0;j<8;j++)crc=(crc>>>1)^((crc&1)?0xedb88320:0);}
 v.setUint32(20,(crc^0xffffffff)>>>0,true);inspectProgram(bytes);return bytes;
}
export function createProgramPreview(bytes,{seed=null,quantum=64,outputLimits=null,clock=null,grid=null,texts=0}={}){
 if(!integer(quantum,1,64))fail();
 const e=freshCore(bytes,{grid,texts}),schema=e.pp_schema()>>>0,cells=e.pp_cells();
 if(clock!==null){
  if(!exact(clock,['nowMs','timers'])||!Number.isSafeInteger(clock.nowMs)||clock.nowMs<0||!dense(clock.timers,1,8))fail();
  if(!clock.timers.every(t=>exact(t,['id','delayMs','intervalMs','words'])&&integer(t.id,1,65535)&&u32(t.delayMs)&&u32(t.intervalMs)&&(t.intervalMs===0||t.intervalMs>=100)&&dense(t.words,0,4)&&t.words.every(i32))||new Set(clock.timers.map(t=>t.id)).size!==clock.timers.length)fail();
  clock.timers.forEach((t,i)=>{if(!e.pp_timer(i,t.id,t.delayMs,t.intervalMs,t.words.length,...Array.from({length:4},(_,j)=>t.words[j]??0)))fail();});
 }
 if(outputLimits!==null){
  if(!dense(outputLimits,8,8)||!outputLimits.every(q=>exact(q,['min','max'])&&i32(q.min)&&i32(q.max)&&q.min<=q.max))fail();
  outputLimits.forEach((q,i)=>{if(!e.pp_limit(i,q.min,q.max))fail();});
 }
 if(seed!==null){
  if(!exact(seed,['schema','revision','cells','values','outputs'])||seed.schema!==schema||seed.cells!==cells||!u32(seed.revision)||!dense(seed.values,cells,cells)||!seed.values.every(i32)||!dense(seed.outputs,8,8)||!seed.outputs.every(i32))fail();
  e.pp_seed(schema,seed.revision,cells);seed.values.forEach((n,i)=>e.pp_seed_cell(i,n));seed.outputs.forEach((n,i)=>e.pp_seed_output(i,n));
 }
 if(e.pp_start(quantum,seed!==null?1:0)!==0)fail();
 if(clock!==null&&e.pp_clock_start(clock.nowMs>>>0,Math.floor(clock.nowMs/4294967296),clock.timers.length)!==0)fail();
 return Object.freeze({
  enqueue(sequence,words){
   if(!Number.isSafeInteger(sequence)||sequence<1||!dense(words,0,4)||!words.every(i32))fail();
   return RUNTIME[e.pp_enqueue(sequence>>>0,Math.floor(sequence/4294967296),words.length,...Array.from({length:4},(_,i)=>words[i]??0))]??fail();
  },
  pump(){return RUNTIME[e.pp_pump()]??fail();},
  queued(){return e.pp_queued();},
  tick(nowMs,sequence){
   if(!Number.isSafeInteger(nowMs)||nowMs<0||!Number.isSafeInteger(sequence)||sequence<1)fail();
   const status=TIMER[e.pp_tick(nowMs>>>0,Math.floor(nowMs/4294967296),sequence>>>0,Math.floor(sequence/4294967296))]??fail();
   return {status,timerId:e.pp_tick_id(),coalesced:e.pp_tick_coalesced()>>>0,accepted:!!e.pp_tick_accepted(),deadlineExhausted:!!e.pp_tick_exhausted()};
  },
  cancel(){return RUNTIME[e.pp_cancel()]??fail();},
  takeReport(){
   const result=RUNTIME[e.pp_report()];if(result==='idle')return null;if(result!=='ok')fail();
   const sequence=(e.pp_report_high()>>>0)*4294967296+(e.pp_report_low()>>>0);
   return {sequence,result:RESULT[e.pp_report_result()]??fail(),revision:e.pp_report_revision()>>>0,usedFuel:e.pp_report_fuel()};
  },
  snapshot(){
   if(e.pp_snapshot()!==0)fail();
   return {schema,revision:e.pp_revision()>>>0,cells,values:Array.from({length:cells},(_,i)=>e.pp_cell(i)),outputs:Array.from({length:8},(_,i)=>e.pp_output(i))};
  },
 });
}
