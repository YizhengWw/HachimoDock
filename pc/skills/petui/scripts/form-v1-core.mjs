/** Existing native integer/choice editor, shared fixed C/WASM. No IO or saves.
 * Package schema/permission validation remains separate. Only a form runtime
 * instantiates 64 KiB; one cached native instance per current declaration.
 * Every invocation reloads the caller's complete integer snapshot first. */
import {encodedLegacyFormCore} from './form-v1-bytes.mjs';
export const LEGACY_FORM_ENGINE='p4-app-form-v1';
const OPS=['previous','next','decrease','increase','accept','cancel'];
const cache=new WeakMap();let module;
const i32=n=>Number.isInteger(n)&&n>=-2147483648&&n<=2147483647;
const integer=n=>i32(n)&&n>=-1e9&&n<=1e9;
const dense=(a,lo,hi)=>Array.isArray(a)&&a.length>=lo&&a.length<=hi&&Object.keys(a).length===a.length&&Array.from({length:a.length},(_,i)=>Object.hasOwn(a,i)).every(Boolean);
const need=v=>{if(!v)throw Error('Invalid bounded legacy form declaration or snapshot');};
function core(r){
 const f=r.form,names=Object.keys(r.vars||{}),signature=JSON.stringify([f,r.pages,names.map(n=>[n,r.vars[n].type])]);
 let c=cache.get(r);if(c?.signature===signature)return c;
 need(f?.engine===LEGACY_FORM_ENGINE&&dense(f.fields,1,3)&&dense(r.pages,1,4)&&names.length<=8);
 if(!module){module=new WebAssembly.Module(Uint8Array.from(atob(encodedLegacyFormCore),c=>c.charCodeAt(0)));need(WebAssembly.Module.imports(module).length===0);}
 const e=new WebAssembly.Instance(module,{}).exports;need(e.memory.buffer.byteLength===65536);
 const slot=n=>{const i=names.indexOf(n);need(i>=0&&r.vars[n].type==='int');return i;};
 const page=r.pages.findIndex(q=>q.id===f.page);need(page>=0);
 e.f1_begin(f.fields.length,page,slot(f.focus_var),slot(f.active_var));
 for(const [i,q]of f.fields.entries()){
  need(q&&['integer','choice'].includes(q.kind));
  if(q.kind==='integer'){
   need(integer(q.min)&&integer(q.max)&&integer(q.step));e.f1_field(i,slot(q.var),slot(q.draft_var),q.min,q.max,q.step,0);
  }else{
   need(dense(q.options,2,8)&&q.options.every(o=>o&&integer(o.value)&&typeof o.label==='string'));
   e.f1_field(i,slot(q.var),slot(q.draft_var),0,0,0,q.options.length);q.options.forEach((o,j)=>e.f1_option(i,j,o.value));
  }
 }
 need(e.f1_commit()===1);const base=e.f1_values(),view=new DataView(e.memory.buffer);
 c={signature,e,load(s){
  need(s?.vars&&names.every(n=>r.vars[n].type!=='int'||(Object.hasOwn(s.vars,n)&&i32(s.vars[n]))));
  names.forEach((n,i)=>view.setInt32(base+4*i,r.vars[n].type==='int'?s.vars[n]:0,true));
 },store(s){names.forEach((n,i)=>{if(r.vars[n].type==='int')s.vars[n]=view.getInt32(base+4*i,true);});}};
 cache.set(r,c);return c;
}
export function legacyForm(runtime,state,operation,argument){
 const f=runtime.form;if(!f)return false;
 if(operation==='text'&&!f.fields.some(q=>q.id===argument))return '';
 const c=core(runtime),e=c.e;c.load(state);const page=runtime.pages.findIndex(q=>q.id===state.page),op=OPS.indexOf(argument);
 if(operation==='allowed')return !!e.f1_allowed(page,op);
 if(operation==='text'){
  const index=f.fields.findIndex(q=>q.id===argument),q=f.fields[index];
  return q.kind==='choice'?(q.options[e.f1_choice_index(index)]?.label??'—'):String(e.f1_display_value(index));
 }
 let result;
 if(operation==='initialize')result=e.f1_initialize();else if(operation==='cancel')result=e.f1_cancel();else if(operation==='apply')result=e.f1_apply(page,op);
 else throw Error('Unknown native legacy form operation');
 if(result||operation==='initialize')c.store(state);return !!result;
}
