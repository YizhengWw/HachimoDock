/** Data-only writer plan; no effects, grants or IO. */
export const VARIABLE_OWNERS_ENGINE='p4-variable-owners-v1';
const need=value=>{if(!value)throw new TypeError('Invalid variable ownership or native writer collision');};
const object=value=>value!==null&&typeof value==='object'&&!Array.isArray(value);
const name=value=>typeof value==='string'&&/^[A-Za-z0-9_.-]{1,31}$/.test(value);
const exact=(value,keys)=>object(value)&&Object.keys(value).length===keys.length&&keys.every(k=>Object.hasOwn(value,k));

export function variableOwners(runtime){
 const vars=runtime.vars;need(object(vars)&&Object.keys(vars).length<=8);
 if(!Object.hasOwn(runtime,'ownership'))return {mixed:false,host:new Set(runtime.host===true?Object.keys(vars):[])};
 const plan=runtime.ownership;
 need(exact(plan,['engine','host'])&&plan.engine===VARIABLE_OWNERS_ENGINE&&runtime.host===true);
 need(runtime.engine==='p4-bounded-runtime-v4'&&runtime.host_context==='p4-host-context-v1'
  &&runtime.input?.engine==='p4-input-v1'&&runtime.surface?.engine==='p4-app-surface-v1'&&runtime.surface.bindings==='p4-variable-bindings-v1');
 need(!['local','scene','game','media','instrument','data'].some(k=>Object.hasOwn(runtime,k)));
 for(const k of ['form','persist','program','dashboard'])if(Object.hasOwn(runtime,k))need(object(runtime[k]));
 need(Object.keys(vars).length>0&&Object.entries(vars).every(([k,v])=>name(k)&&object(v)&&['int','string'].includes(v.type)));
 need(Array.isArray(plan.host)&&plan.host.length>0&&plan.host.length<=Object.keys(vars).length);
 const host=new Set(plan.host);need(host.size===plan.host.length&&plan.host.every(k=>name(k)&&Object.hasOwn(vars,k)));
 const owns=k=>typeof k==='string'&&host.has(k);
 function nativeRefs(value,depth=0){
  need(depth<=16);
  if(!object(value)&&!Array.isArray(value))return;
  for(const [k,v] of Object.entries(value)){
   need(!['var','fmt_mmss','fmt_hms','pct_of','of_max'].includes(k)||!owns(v));
   nativeRefs(v,depth+1);
  }
 }
 for(const key of ['transitions','tick']){
  need(!Object.hasOwn(runtime,key)||Array.isArray(runtime[key]));
  for(const rule of runtime[key]||[]){
   need(object(rule));
   if(Object.hasOwn(rule,'then'))need(object(rule.then));
   if(key==='transitions'&&typeof rule.on==='string'&&rule.on.startsWith('host.'))need(exact(rule,['from','on'])&&rule.from==='*');
   nativeRefs(rule);
   for(const body of [rule,rule.then])if(body!=null)for(const op of ['set','inc'])if(Object.hasOwn(body,op)){
    need(object(body[op])&&!Object.keys(body[op]).some(owns));
   }
  }
 }
 nativeRefs(runtime.dashboard);
 if(runtime.persist&&Object.hasOwn(runtime.persist,'vars'))need(Array.isArray(runtime.persist.vars)&&!runtime.persist.vars.some(owns));
 const f=runtime.form;
 if(f&&Object.hasOwn(f,'keyboard'))need(object(f.keyboard));
 need(!['focus_var','active_var'].some(k=>owns(f?.[k]))&&!['index_var','label_var'].some(k=>owns(f?.keyboard?.[k])));
 if(f&&Object.hasOwn(f,'fields'))need(Array.isArray(f.fields)&&f.fields.every(object)&&!f.fields.some(v=>owns(v.var)||owns(v.draft_var)));
 if(Object.hasOwn(runtime,'program'))need(Array.isArray(runtime.program?.outputs)&&runtime.program.outputs.length===8&&!runtime.program.outputs.some(owns));
 return {mixed:true,host};
}

/* Call after schema validation. These bounded name checks neither allocate a
 * VM nor grant access to services. Pure local components do not load WASM. */
export function hostVariable(runtime,key){
 return runtime.host===true&&(!runtime.ownership||runtime.ownership.host.includes(key));
}
export function hostAction(runtime,action){
 return runtime.host===true&&(!runtime.ownership||typeof action==='string'&&action.startsWith('host.'));
}
export function programHostDependencies(runtime,action){
 const words=runtime.program?.actions?.[action];
 if(!Array.isArray(words))return [];
 return [...new Set(words.filter(w=>object(w)&&hostVariable(runtime,w.var)).map(w=>w.var))];
}
function bounded(runtime,key,value){
 const [lo,hi]=runtime.limits?.[key]||[-1e9,1e9];return Math.max(lo,Math.min(hi,value));
}
/* Immutable reply supplied by the trusted caller: {fresh,variables}. It is
 * captured ONCE by the action handler; no per-variable re-read or fallback.
 * Returning null is absence/backpressure, never an event submission receipt. */
export function programInputValues(runtime,native,action,reply){
 const names=programHostDependencies(runtime,action);
 if(!names.length)return {...native};
 if(reply?.fresh!==true||!object(reply.variables))return null;
 const values={...native};
 for(const key of names){
  const value=reply.variables[key];
  if(!Number.isInteger(value)||value< -1e9||value>1e9)return null;
  values[key]=bounded(runtime,key,value);
 }
 return values;
}
/* Presentation is separate from the committed native model: replies can
 * replace only marked HOST fields, never program/form/save values. */
export function presentedVariables(runtime,native,reply){
 const values={...native};
 for(const [key,def] of Object.entries(runtime.vars||{}))if(hostVariable(runtime,key)){
  const value=reply?.fresh===true?reply.variables?.[key]:undefined;
  if(def.type==='int')values[key]=Number.isInteger(value)&&value>=-1e9&&value<=1e9?bounded(runtime,key,value):0;
  else values[key]=typeof value==='string'?value:'—';
 }
 return values;
}

/* A presentation snapshot is never fed back into native transitions, saves or
 * the VM. Polling replaces only this immutable view, not the live instance. */
export function mixedHostSnapshot(runtime,native,reply){
 if(!runtime.ownership)return native;
 const fresh=reply?.fresh===true;
 return {...native,vars:presentedVariables(runtime,native.vars,reply),hostReply:reply,
  hostEpoch:JSON.stringify([fresh,reply?._hostEpoch||0,hostFingerprint(runtime,reply)]),
  values:fresh?reply.values||{}:{},collectionView:fresh?reply.collectionView:null,
  ui:fresh?reply.ui:{mode:7,actions:[]}};
}

function hostFingerprint(runtime,reply){
 return JSON.stringify([runtime.ownership.host.map(k=>[k,reply?.variables?.[k]??null]),
  reply?.ui?.mode??null,reply?.ui?.context??null,reply?.ui?.actions??null,reply?.collectionView??null]);
}
export function updateOwnedHostReply(runtime,previous,reply,merge=false){
 const next=merge?{...previous,...reply}:{...reply};
 const before=previous?._hostEpoch||0;
 if(!Number.isSafeInteger(before)||before<0||before===Number.MAX_SAFE_INTEGER)
  return {...next,enabled:false,_hostEpoch:Number.MAX_SAFE_INTEGER,ui:{mode:7,actions:[]}};
 return {...next,_hostEpoch:before+(hostFingerprint(runtime,previous)!==hostFingerprint(runtime,next)?1:0)};
}

export function hostDependent(runtime,action,node){
 if(!runtime.host)return false;
 if(!runtime.ownership)return true;
 if(hostAction(runtime,action)||programHostDependencies(runtime,action).length)return true;
 if(node?.host_mask||node?._collectionSlot!==undefined||hostVariable(runtime,node?.show?.var))return true;
 const views=runtime.surface?.viewports?.items||(runtime.surface?.viewport?[runtime.surface.viewport]:[]);
 return views.some(v=>hostVariable(runtime,v.offset_var));
}
export function hostGeometryReady(runtime,snapshot,action,node){
 return !runtime.ownership||!hostDependent(runtime,action,node)||snapshot.hostReply?.fresh===true;
}
