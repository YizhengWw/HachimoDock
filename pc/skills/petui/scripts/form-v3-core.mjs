// Device form-v3 core, fixed product WASM; never package code.
import {encodedFormCore} from './form-v3-bytes.mjs';
export const TEXT_FORM_ENGINE='p4-app-form-v3';
const OPS=['previous','next','decrease','increase','accept','cancel','append','erase'];
const utf8=new TextEncoder(),decode=new TextDecoder('utf-8',{fatal:true});
const int=(n,lo,hi)=>Number.isInteger(n)&&n>=lo&&n<=hi;
const safeText=(s)=>typeof s==='string'&&utf8.encode(s).length<=63&&![...s].some(c=>{const n=c.codePointAt(0);return n<32||(n>=127&&n<160)||(n>=0xd800&&n<=0xdfff);});
const cache=new WeakMap();let module;
function core(r){
 const signature=JSON.stringify([r.form,r.vars,r.pages]);let cached=cache.get(r);
 if(cached?.signature===signature)return cached;

 if(!module){const data=Uint8Array.from(atob(encodedFormCore),c=>c.charCodeAt(0));module=new WebAssembly.Module(data);if(WebAssembly.Module.imports(module).length)throw Error('Unexpected form imports');}
 const e=new WebAssembly.Instance(module,{}).exports,f=r.form,k=f.keyboard,names=[...new Set([f.focus_var,f.active_var,...f.fields.flatMap(q=>[q.var,q.draft_var]),...Object.values(k)])],slot=n=>names.indexOf(n),string=s=>{const bytes=utf8.encode(s);if(bytes.length>63)throw Error('Form input exceeds bounded memory');new Uint8Array(e.memory.buffer,e.fv3_buffer(),128).set(bytes);};
 e.fv3_begin(f.fields.length,r.pages.findIndex(p=>p.id===f.page),slot(f.focus_var),slot(f.active_var),slot(k.key_var),slot(k.cursor_var),slot(k.buffer_var),slot(k.label_var),slot(k.candidates_var));
 f.fields.forEach((q,i)=>e.fv3_field(i,slot(q.var),slot(q.draft_var),['numeric','latin','pinyin'].indexOf(q.keyboard),q.max_bytes));
 if(!e.fv3_commit())throw Error('Native text form configuration refused');
 const textAt=(address,capacity)=>{const bytes=new Uint8Array(e.memory.buffer,address,capacity),end=bytes.indexOf(0);if(end<0)throw Error('Invalid form text terminator');return decode.decode(bytes.subarray(0,end));};
 cached={signature,e,names,string,textAt,load(s){
  // Validate the entire typed snapshot before changing any core state.
  for(const n of names)if(!(r.vars[n].type==='int'?int(s.vars[n],-2147483648,2147483647):safeText(s.vars[n])))throw Error('Invalid typed form snapshot');
  e.fv3_clear_values();names.forEach((n,i)=>{const text=r.vars[n].type==='string';if(text)string(s.vars[n]);e.fv3_value(i,text?0:s.vars[n],text?1:0);});
 },store(s){const base=e.fv3_values(),view=new DataView(e.memory.buffer);names.forEach((n,i)=>{s.vars[n]=r.vars[n].type==='string'?textAt(base+i*68+4,64):view.getInt32(base+i*68,true);});}};
 cache.set(r,cached);return cached;
}
export function textForm(runtime,state,operation,argument){
 const c=core(runtime),e=c.e;c.load(state);const page=runtime.pages.findIndex(p=>p.id===state.page),op=OPS.indexOf(argument);
 if(operation==='allowed')return !!e.fv3_allowed(page,op);
 if(operation==='text'){const field=runtime.form.fields.findIndex(q=>q.id===argument);return c.textAt(e.fv3_display(field),121);}
 const before=JSON.stringify(state.vars);let result;
 if(operation==='initialize')result=e.fv3_initialize();else if(operation==='cancel')result=e.fv3_cancel();else if(operation==='apply')result=e.fv3_apply(page,op);
 else throw Error('Unknown bounded form operation');
 if(operation==='initialize'&&!result)throw Error('Unable to initialize typed form state');
 if(result){c.store(state);if(before!==JSON.stringify(state.vars))state.formEpoch=(state.formEpoch||0)+1;}return !!result;
}
export function replaceTextDraft(runtime,state,field,text){
 if(!safeText(text))return 'invalid';const c=core(runtime);c.load(state);c.string(text);
 const result=c.e.fv3_replace(runtime.pages.findIndex(p=>p.id===state.page),runtime.form.fields.findIndex(q=>q.id===field));
 if(result===0){c.store(state);state.formEpoch=(state.formEpoch||0)+1;}
 return ['applied','unchanged','denied','invalid','limit'][result]??'invalid';
}
