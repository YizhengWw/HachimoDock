/** Data-only component adapter over the same product C interpreter. No IO. */
import {encodeProgram,createProgramPreview} from './program-core.js';
import {variableOwners} from './variable-owners.js';
export const COMPONENT_PROGRAM_ENGINE='p4-component-program-v1';
const need=q=>{if(!q)throw new TypeError('Invalid bounded component program or output ownership');};
const object=(v,keys)=>need(v&&typeof v==='object'&&!Array.isArray(v)&&Object.keys(v).length===keys.length&&keys.every(k=>Object.hasOwn(v,k)));
const integer=(v,lo,hi)=>Number.isInteger(v)&&v>=lo&&v<=hi;
const i32=v=>integer(v,-2147483648,2147483647);
const name=(v,max)=>typeof v==='string'&&v.length>0&&v.length<=max&&[...v].every(c=>{const n=c.charCodeAt(0);return n>=65&&n<=90||n>=97&&n<=122||n>=48&&n<=57||'._-'.includes(c);});
export function componentProgramConfig(r){
  if(!Object.hasOwn(r,'program'))return null;
  const p=r.program;object(p,['engine','image','outputs','actions','timers']);
  need([COMPONENT_PROGRAM_ENGINE,'p4-component-program-v2'].includes(p.engine)&&r.engine==='p4-bounded-runtime-v4'&&r.surface?.engine==='p4-app-surface-v1'&&r.input?.engine==='p4-input-v1');
  const writers=variableOwners(r);
  need((!Object.hasOwn(r,'host')||writers.mixed)&&!['local','scene','game','media','instrument','data'].some(k=>Object.hasOwn(r,k)));
  const bytes=encodeProgram(p.image),vars=r.vars;
  const imageV2=p.image.engine==='p4-integer-program-v2';need(!imageV2||p.engine==='p4-component-program-v2');
  const grid=r.cells?{cols:r.cells.cols,rows:r.cells.rows}:null;
  if(grid)need(imageV2&&integer(grid.cols,1,16)&&integer(grid.rows,1,16)&&grid.cols*grid.rows<=p.image.cells);
  if(p.image.code.some(i=>['CELLGET','CELLSET'].includes(i[0])))need(grid);
  if(Object.hasOwn(r,'texts'))need(imageV2&&Array.isArray(r.texts)&&r.texts.length>=1&&r.texts.length<=64);
  need(vars&&typeof vars==='object'&&!Array.isArray(vars)&&Object.keys(vars).length>=1&&Object.keys(vars).length<=8&&Object.entries(vars).every(([k,v])=>name(k,31)&&v&&['int','string'].includes(v.type)));
  need(Array.isArray(p.outputs)&&p.outputs.length===8);
  const owned=p.outputs.filter(k=>k!==null);need(owned.length&&new Set(owned).size===owned.length&&owned.every(k=>name(k,31)&&vars[k]?.type==='int'));
  for(const [k,v] of Object.entries(vars))if(v.type==='int'&&Object.hasOwn(r.limits||{},k)){
    const q=r.limits[k];need(Array.isArray(q)&&q.length===2&&integer(q[0],-1e9,1e9)&&integer(q[1],q[0],1e9));
  }
  for(const rule of [...r.transitions||[],...r.tick||[]])for(const body of [rule,rule.then||{},rule.else||{}])need(![...Object.keys(body.set||{}),...Object.keys(body.inc||{})].some(k=>owned.includes(k)));
  need(!r.persist?.vars?.some(k=>owned.includes(k)));
  need(!['focus_var','active_var'].some(k=>owned.includes(r.form?.[k]))&&!r.form?.fields?.some(f=>['var','draft_var'].some(k=>owned.includes(f[k]))));
  need(!['index_var','label_var'].some(k=>owned.includes(r.form?.keyboard?.[k])));
  need(p.actions&&typeof p.actions==='object'&&!Array.isArray(p.actions)&&Object.keys(p.actions).length>=1&&Object.keys(p.actions).length<=24);
  for(const [action,words] of Object.entries(p.actions)){
    need(!Object.values(r.form?.actions||{}).includes(action));
    const rules=r.transitions?.filter(t=>t.on===action);need(name(action,47)&&!action.startsWith('host.')&&rules?.length&&rules.every(t=>Object.keys(t).length===2&&t.from==='*'));
    need(Array.isArray(words)&&words.length<=4);
    for(const word of words)if(!i32(word)){object(word,['var']);need(typeof word.var==='string'&&vars[word.var]?.type==='int');}
  }
  need(Array.isArray(p.timers)&&p.timers.length<=8);const ids=new Set();
  for(const t of p.timers){object(t,['id','delayMs','intervalMs','words']);need(integer(t.id,1,65535)&&!ids.has(t.id)&&integer(t.delayMs,0,4294967295)&&integer(t.intervalMs,0,4294967295)&&(t.intervalMs===0||t.intervalMs>=100)&&Array.isArray(t.words)&&t.words.length<=4&&t.words.every(i32));ids.add(t.id);}
  return {bytes,grid,texts:r.texts?.length||0,definition:structuredClone(p),limits:p.outputs.map(k=>k===null?{min:-2147483648,max:2147483647}:{min:r.limits?.[k]?.[0]??-1e9,max:r.limits?.[k]?.[1]??1e9})};
}
export function createComponentProgram(runtime,{seed=null,nowMs=0}={}){
  const config=componentProgramConfig(runtime);need(config&&Number.isSafeInteger(nowMs)&&nowMs>=0);
  const p=config.definition;
  const initial=seed||{schema:p.image.schema,revision:0,cells:p.image.cells,values:Array.from({length:p.image.cells},(_,i)=>runtime.cells&&i<runtime.cells.cols*runtime.cells.rows?parseInt(runtime.cells.init?.[Math.floor(i/runtime.cells.cols)]?.[i%runtime.cells.cols]||'0',16):0),outputs:p.outputs.map(k=>k===null?0:runtime.vars[k].init??0)};
  const core=createProgramPreview(config.bytes,{seed:initial,quantum:64,grid:config.grid,texts:config.texts,outputLimits:config.limits,clock:p.timers.length?{nowMs,timers:p.timers}:null});
  let live=true,sequence=0,lastReport=null,phase='ok';
  const stop=()=>{if(live)core.cancel();live=false;phase='stopped';return phase;};
  return Object.freeze({
    handles:action=>Object.hasOwn(p.actions,action),
    enqueue(action,values){
      if(!Object.hasOwn(p.actions,action))return 'unhandled';if(!live)return 'stopped';
      need(values&&typeof values==='object');const words=p.actions[action].map(w=>typeof w==='number'?w:values[w.var]);need(words.every(i32));
      if(sequence===Number.MAX_SAFE_INTEGER)return stop();
      phase=core.enqueue(sequence+1,words);if(phase==='ok')sequence++;else if(!['busy','queue_full'].includes(phase))stop();return phase;
    },
    pump(base,now){
      need(base&&typeof base==='object'&&Number.isSafeInteger(now)&&now>=0);
      if(!live)return null;
      if(p.timers.length){
        if(sequence===Number.MAX_SAFE_INTEGER){stop();return null;}
        const tick=core.tick(now,sequence+1);if(tick.accepted)sequence++;
        if(!['ok','idle','enqueued','backpressure'].includes(tick.status)){stop();return null;}
      }
      phase=core.pump();if(phase==='complete'){
        lastReport=core.takeReport();
        if(lastReport?.result!=='committed'){stop();return null;}
        const snapshot=core.snapshot(),out={...base};p.outputs.forEach((k,i)=>{if(k!==null)out[k]=snapshot.outputs[i];});return out;
      }
      if(['stopped','denied','stale'].includes(phase))stop();return null;
    },
    cancel:stop,snapshot:()=>core.snapshot(),
    gridSnapshot:()=>{if(!runtime.cells)return null;const v=core.snapshot().values,{cols,rows,bits}=runtime.cells,max=(1<<bits)-1;return Array.from({length:rows},(_,y)=>Array.from({length:cols},(_,x)=>Math.max(0,Math.min(max,v[y*cols+x])).toString(16)).join(''));},
    diagnostic:()=>({live,phase,sequence,queued:core.queued(),canEnqueue:live&&sequence<Number.MAX_SAFE_INTEGER&&core.queued()<4,lastReport:lastReport?{...lastReport}:null}),
  });
}
