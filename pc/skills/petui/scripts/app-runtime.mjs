/** Bounded v4 tool simulator. Preview never calls host services or writes device saves. */
import {validateFormWriters,validateDerivedWriters} from './app-writers.mjs';
import {validateRuntimeTexts} from './texts-schema.mjs';
import {validateBridgeSurface} from "./service-bridge.mjs";
import {actionEnabled,actionAccepted} from "./input-policy.mjs";
import {localInitial,localAction,localAdvance,localView} from './local-tools.mjs';
import {hitNode,viewportContext} from './app-viewport.mjs';
import {navigate} from './app-navigation.mjs';
import {cellGet,conditionHolds,selectTransition} from './app-transitions.mjs';
export {cellGet,conditionHolds} from './app-transitions.mjs';
import {formOperation,formApply,formCancel,formInitialize,formText} from './app-form.mjs';
import {deviceJsonStringsValidText,deviceJsonWithinDepth,DEVICE_JSON_MAX_DEPTH} from './device-json.mjs';
import {persistenceFits} from './store-budget.mjs';
import {dSnapshot} from './d-surface.mjs';
const bounded = n => Math.min(1e9, Math.max(-1e9, n));
// Row strings are the portable grid representation; every write clamps like pet_cells_set.
const cellArg=(v,vars)=>typeof v==='object'?vars[v.var]:v;
function cellRun(rows,x,y,value){
 if(cellGet(rows,x,y)!==value)return 0;let best=1;
 for(const [dx,dy] of [[1,0],[0,1],[1,1],[1,-1]]){let n=1;for(const sign of [-1,1])for(let k=1;k<=16;k++){if(cellGet(rows,x+sign*k*dx,y+sign*k*dy)!==value)break;n++;}best=Math.max(best,n);}return best;
}
function cellEffects(runtime,s,ops){
 if(!runtime.cells||!s.cells)return;
 const put=(x,y,v)=>{if(cellGet(s.cells,x,y)<0)return;const d=Math.max(0,Math.min((1<<runtime.cells.bits)-1,v)).toString(16);s.cells[y]=s.cells[y].slice(0,x)+d+s.cells[y].slice(x+1);};
 for(const o of ops||[]){const v=cellArg(o.set??o.value,s.vars),x=cellArg(o.x??0,s.vars),y=cellArg(o.y??0,s.vars);
  if(o.op==='cell')put(x,y,v);
  else if(o.op==='cellfill'){for(let y=0;y<s.cells.length;y++)for(let x=0;x<s.cells[y].length;x++)put(x,y,v);}
  else {const n=o.op==='cellcount'?[...s.cells.join('')].filter(c=>parseInt(c,16)===v).length:cellRun(s.cells,x,y,v);const [lo,hi]=runtime.limits?.[o.var]||[-1e9,1e9];s.vars[o.var]=Math.max(lo,Math.min(hi,n));}
 }
}
export function textAt(texts,index){return Number.isInteger(index)&&index>=0&&index<(texts?.length||0)?texts[index]:'';}
export function initialApp(runtime) {
  validateFormWriters(runtime);validateDerivedWriters(runtime);validateRuntimeTexts(runtime);
  const serviceErrors=validateBridgeSurface(runtime.surface,runtime);if(serviceErrors.length)throw new TypeError(serviceErrors[0]);
  if(!persistenceFits(runtime))throw new TypeError("Persist subset exceeds its worst-case JSON budget");
  if(!deviceJsonStringsValidText(runtime))throw new TypeError('Device JSON strings cannot contain NUL or unpaired Unicode surrogates');
  if(!deviceJsonWithinDepth(runtime))throw new TypeError('Device JSON exceeds container depth budget ('+DEVICE_JSON_MAX_DEPTH+')');
  const s={state:runtime.initial_state,page:runtime.initial_page||runtime.pages?.[0]?.id||"",vars:Object.fromEntries(Object.entries(runtime.vars||{}).map(([k,d])=>[k,d.init??(d.type==="int"?0:"")])),clock:0,ticks:(runtime.tick||[]).map(()=>0)};
  if(runtime.cells)s.cells=runtime.cells.init?.map(r=>r.toLowerCase())||Array(runtime.cells.rows).fill("0".repeat(runtime.cells.cols));
  // Store derived counts before the first guest timer reads its input snapshot.
  return dSnapshot(runtime,runtime.local?localAdvance(runtime,localInitial(s)):formInitialize(runtime,s));
}
function effects(runtime,s,r) {
  const beforeScroll=viewportContext(runtime.surface,s.vars);
  const original={...s.vars};
  const clamp=(k,v)=>{if(typeof v!=="number")return v;const [lo,hi]=runtime.limits?.[k]||[-1e9,1e9];return Math.max(lo,Math.min(hi,v));};
  for(const [k,v] of Object.entries(r.set||{}))s.vars[k]=clamp(k,v&&typeof v==="object"?original[v.var]:v);
  for(const [k,v] of Object.entries(r.inc||{}))s.vars[k]=clamp(k,bounded(Number(s.vars[k])+(v&&typeof v==="object"?Number(original[v.var]):v)));
  for(const [k,[lo,hi]] of Object.entries(runtime.limits||{}))s.vars[k]=Math.min(hi,Math.max(lo,s.vars[k]));
  cellEffects(runtime,s,r.cells);
  if(beforeScroll!==viewportContext(runtime.surface,s.vars))s.pointerEpoch=(s.pointerEpoch||0)+1;
}
export function appAction(runtime,state,action) {
  if(!actionEnabled(runtime,state,action))return state;
  if(runtime.local){const next=localAction(runtime,state,action);return actionAccepted(runtime,next,action,next!==state);}
  const op=formOperation(runtime,action);
  if(op){const s=structuredClone(state);formApply(runtime,s,op);return s;}
  // A false guard without else behaves like an unavailable navigation rule: keep searching.
  const r=selectTransition(runtime,state,action);if(!r)return state;
  const s=formCancel(runtime,structuredClone(state));if(r.to&&r.to!==s.state){s.state=r.to;s.ticks.fill(s.clock);}
  navigate(runtime,s,r);
  // React may batch several actions into one render. Remember intermediate
  // geometry changes even when a page/offset returns to its starting value.
  // This is transient preview state, never a component var or saved field.
  if(s.page!==state.page||s.state!==state.state)s.pointerEpoch=(s.pointerEpoch||0)+1;
  effects(runtime,s,r);
  if(r.sfx)queueCue(s,r.sfx);
  if(runtime.form&&runtime.form.fields.some(q=>s.vars[q.var]!==state.vars[q.var]))s.formEpoch=(s.formEpoch||0)+1;
  return actionAccepted(runtime,s,action);
}
/* p4-sfx-v1: cues queued by transitions and tick completions, like the firmware
   sfx_pending bitmask (a cue repeats at most once per drain). The host drains
   them with takeCues; they are transient preview state, never a component var. */
function queueCue(s,cue){s.cues=s.cues||[];if(!s.cues.includes(cue))s.cues.push(cue);}
export function takeCues(state){const cues=state.cues||[];state.cues=[];return cues;}
export function appAdvance(runtime,state,ms) {
  const s=structuredClone(state);s.clock+=Math.max(0,ms);
  if(runtime.local)return localAdvance(runtime,s);
  (runtime.tick||[]).forEach((t,i)=>{
    // An omitted while_state is global, just as in the Python model and P4
    // parser (-1). A literal "*" is not a valid state in the package contract.
    if(t.while_state!=null&&t.while_state!==s.state){s.ticks[i]=s.clock;return;}
    const steps=Math.min(10,Math.floor((s.clock-s.ticks[i])/t.every_ms));
    for(let n=0;n<steps;n++){
      effects(runtime,s,t);s.ticks[i]+=t.every_ms;
      if(t.when&&conditionHolds(t.when,s.vars,s.cells)){
        if(t.then?.to&&t.then.to!==s.state){s.state=t.then.to;s.pointerEpoch=(s.pointerEpoch||0)+1;}
        effects(runtime,s,t.then||{});if(t.then?.sfx)queueCue(s,t.then.sfx);s.ticks.fill(s.clock);break;
      }
    }
  });return s;
}
/* Firmware parse_value: a leaf is a literal, {var}, {fmt_mmss} or {fmt_hms}. Switch
   branches are leaves too; a nested switch is rejected by the device, so it must not
   resolve in the preview either. */
function leaf(rule,s,texts) {
  if(typeof rule==="string")return rule;
  if(!rule||typeof rule!=="object")return "";
  if(typeof rule.var==="string"&&rule.var)return String(s.vars[rule.var]??"");
  if(typeof rule.text_at==="string")return textAt(texts,s.vars[rule.text_at]);
  const name=rule.fmt_mmss||rule.fmt_hms;
  if(typeof name==="string"&&name){const n=Math.max(0,Number(s.vars[name])||0),ss=String(n%60).padStart(2,"0"),mm=String(Math.floor(n/60)%(rule.fmt_hms?60:1e9)).padStart(2,"0");return rule.fmt_hms?`${String(Math.floor(n/3600)).padStart(2,"0")}:${mm}:${ss}`:`${mm}:${ss}`;}
  return "";
}
function value(rule,s,texts) {
  if(rule&&typeof rule==="object"){
    if(rule.switch_state)return leaf(rule.switch_state[s.state],s,texts);
    if(rule.switch_page)return leaf(rule.switch_page[s.page],s,texts);
  }
  return leaf(rule,s,texts);
}
export function appDashboard(runtime,state) {
  if(runtime.local)return {...runtime.dashboard,...localView(runtime,state)};
  return Object.fromEntries(Object.entries(runtime.dashboard||{}).map(([k,r])=>[k,k==="progress"?Math.max(0,Math.min(100,Number(state.vars[r?.pct_of])*100/(Number(state.vars[r?.of_max])||1))) :value(r,state,runtime.texts)]));
}
export function appPage(runtime,state){return Math.max(0,(runtime.pages||[]).findIndex(p=>p.id===state.page));}
export function appNodeValue(node,snapshot,values={},runtime={}){
  if(node.text_at)return textAt(runtime.texts,snapshot.vars?.[node.text_at]);
  if(node.field_bind)return formText(runtime,snapshot,node.field_bind);
  return node.var_bind?(snapshot.vars?.[node.var_bind]??""):node.bind?(values[node.bind]??""):(node.text??"");
}
export function appNodeMeter(node,snapshot,values={}){
  const value=Number(node.var_bind?appNodeValue(node,snapshot,values):values.progress);
  return Number.isFinite(value)?Math.max(0,Math.min(100,value)):0;
}
export function appVisible(n,page,vars={},stateIndex=0,hostMode=0){return !n.hidden&&(!n.host_mask||!!(n.host_mask&(1<<hostMode)))&&(!n.state_mask||!!(n.state_mask&(1<<stateIndex)))&&(n.page==null||n.page===4||n.page===page)&&(!n.show||(Number.isFinite(vars[n.show.var])&&vars[n.show.var]>=n.show.min&&vars[n.show.var]<=n.show.max));}
export function appTile(n,vars={}){const q=n.tile;if(!q)return null;const v=Number(vars[q.var])||0,c=q.columns,max=Number(vars[q.max_var]);return {columns:c,rows:q.rows,row:q.row,index:q.max_var?Math.min(c-1,Math.max(0,max>0?Math.floor(v*c/max):0)):((v%c)+c)%c};}
export function appHit(surface,page,x,y,vars={},stateIndex=0,hostMode=0){return [...(surface?.elements||[])].reverse().find(n=>n.action&&appVisible(n,page,vars,stateIndex,hostMode)&&hitNode(surface,n,vars,x,y))?.action||null;}
