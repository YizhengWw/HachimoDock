/** Shared bounded native text editor. No package code, network or storage. */
import {encodedFormCore} from './form-v2-bytes.mjs';
import {variableOwners} from './variable-owners.mjs';
export const MIXED_FORM_ENGINE='p4-app-form-v2';
const OPS=['previous','next','decrease','increase','accept','cancel','append','erase'];
const utf8=new TextEncoder(),decode=new TextDecoder('utf-8',{fatal:true});
const safeText=(s,max=63,empty=true)=>typeof s==='string'&&(empty||s.length>0)&&utf8.encode(s).length<=max&&![...s].some(c=>{const n=c.codePointAt(0);return n<32||(n>=127&&n<160)||(n>=0xd800&&n<=0xdfff);});
const id=(s,n)=>typeof s==='string'&&s.length>=1&&s.length<=n&&!/[^A-Za-z0-9_.-]/.test(s);
const int=(n,lo=-1e9,hi=1e9)=>Number.isInteger(n)&&n>=lo&&n<=hi;
const keys=(o,allowed,required=allowed)=>o&&typeof o==='object'&&!Array.isArray(o)&&Object.keys(o).every(k=>allowed.includes(k))&&required.every(k=>Object.hasOwn(o,k));
const need=v=>{if(!v)throw Error('Invalid bounded mixed form declaration');};
const noNul=v=>typeof v==='string'?!v.includes('\0'):Array.isArray(v)?v.every(noNul):v&&typeof v==='object'?Object.entries(v).every(([k,v])=>noNul(k)&&noNul(v)):true;
export function validateMixedForm(r,buttons){
 const f=r?.form,v=r?.vars,s=r?.surface,p=r?.input;
 need(noNul(r)&&keys(f,['engine','page','focus_var','active_var','fields','actions','keyboard'])&&f.engine===MIXED_FORM_ENGINE);
 need(r.engine==='p4-bounded-runtime-v4'&&s?.engine==='p4-app-surface-v1'&&s.graphics==='p4-app-graphics-v1'&&s.bindings==='p4-variable-bindings-v1'&&p?.engine==='p4-input-v1');
 need((!Object.hasOwn(r,'host')||variableOwners(r).mixed)&&!['local','scene','game','data','media','instrument'].some(k=>Object.hasOwn(r,k))&&Array.isArray(r.pages)&&int(r.pages.length,1,4)&&r.pages.some(q=>q.id===f.page));
 need(v&&typeof v==='object'&&!Array.isArray(v)&&Object.keys(v).length<=8&&Array.isArray(f.fields)&&int(f.fields.length,1,3));
 need(r.pages.every(q=>keys(q,['id'])&&id(q.id,23))&&new Set(r.pages.map(q=>q.id)).size===r.pages.length);
 const owned=new Set(),ephemeral=new Set(),ids=new Set();
 function role(n,type,expected,bounds,transient=true){
  need(id(n,31)&&!owned.has(n)&&v[n]?.type===type);const initial=Object.hasOwn(v[n],'init')?v[n].init:(type==='int'?0:'');
  need(type==='int'?int(initial):safeText(initial));if(expected!==undefined)need(initial===expected);
  if(bounds&&r.limits?.[n])need(Array.isArray(r.limits[n])&&r.limits[n].length===2&&int(r.limits[n][0],-1e9,bounds[0])&&int(r.limits[n][1],bounds[1],1e9));
  if(type==='string')need(!Object.hasOwn(r.limits||{},n));owned.add(n);if(transient)ephemeral.add(n);return initial;
 }
 role(f.focus_var,'int',0,[0,f.fields.length-1]);role(f.active_var,'int',0,[0,1]);
 for(const q of f.fields){
  need(id(q?.id,15)&&!ids.has(q.id));ids.add(q.id);const base=['id','var','draft_var','kind'];
  if(q.kind==='text'){
   need(keys(q,[...base,'max_bytes'])&&int(q.max_bytes,1,63));const init=role(q.var,'string',undefined,undefined,false);need(safeText(init,q.max_bytes));role(q.draft_var,'string',init);
  }else{
   let bounds;
   if(q.kind==='integer'){need(keys(q,[...base,'min','max','step'])&&int(q.min)&&int(q.max)&&q.min<q.max&&int(q.step,1,1e9)&&(q.max-q.min)%q.step===0);bounds=[q.min,q.max];}
   else{need(q.kind==='choice'&&keys(q,[...base,'options'])&&Array.isArray(q.options)&&int(q.options.length,2,8));const values=new Set();for(const o of q.options){need(keys(o,['value','label'])&&int(o.value)&&!values.has(o.value)&&safeText(o.label,63,false));values.add(o.value);}bounds=[Math.min(...values),Math.max(...values)];}
   const init=role(q.var,'int',undefined,bounds,false);need(q.kind==='choice'?q.options.some(o=>o.value===init):init>=q.min&&init<=q.max&&(init-q.min)%q.step===0);role(q.draft_var,'int',init,bounds);
  }
 }
 need(f.fields.some(q=>q.kind==='text'));const k=f.keyboard;
 need(keys(k,['index_var','label_var','characters','space_label'],['index_var','label_var','characters'])&&Array.isArray(k.characters)&&int(k.characters.length,1,96));
 need(k.characters.every(c=>safeText(c,4,false)&&[...c].length===1)&&new Set(k.characters).size===k.characters.length);
 const space=Object.hasOwn(k,'space_label')?k.space_label:'Space';need(safeText(space,16,false));role(k.index_var,'int',0,[0,k.characters.length-1]);role(k.label_var,'string',k.characters[0]===' '?space:k.characters[0]);
 for(const [name,d] of Object.entries(v)){need(id(name,31)&&keys(d,['type','init'],['type']));const init=Object.hasOwn(d,'init')?d.init:(d.type==='int'?0:'');need(d.type==='int'?int(init):d.type==='string'&&safeText(init));}
 need(keys(f.actions,OPS)&&new Set(Object.values(f.actions)).size===8&&Array.isArray(r.transitions));
 const reserved=['page_toggle','page_enter','page_back','page_main','page_app','component_center'];
 for(const a of Object.values(f.actions)){
  need(id(a,47)&&!a.startsWith('input.')&&!a.startsWith('host.')&&!reserved.includes(a));const matching=r.transitions.filter(t=>t.on===a);
  need(matching.length===1&&keys(matching[0],['from','on'])&&matching[0].from==='*'&&!p.press?.includes(a)&&p.hold!==a&&!Object.hasOwn(p.confirm||{},a)&&!Object.hasOwn(p.cooldown||{},a));
 }
 need(!['accept','cancel','append','erase'].some(op=>p.repeat?.includes(f.actions[op])));
 const safe=(rule,set)=>['set','inc'].every(op=>!Object.hasOwn(rule,op)||(keys(rule[op],Object.keys(v),[])&&!Object.keys(rule[op]).some(k=>set.has(k))));
 need(r.transitions.every(t=>safe(t,owned)&&(!t.else||safe(t.else,owned)))&&(!Object.hasOwn(r,'tick')||(Array.isArray(r.tick)&&r.tick.every(t=>safe(t,owned)&&(!t.then||safe(t.then,owned))))));
 need(!(r.persist?.vars||[]).some(n=>ephemeral.has(n)));
 need((s.elements||[]).every(n=>!Object.hasOwn(n,'field_bind')||(ids.has(n.field_bind)&&['text','button'].includes(n.type)&&!['text','bind','var_bind','key_for'].some(k=>Object.hasOwn(n,k)))));
 if(buttons!==undefined){const physical=['button.sw1.short_press','button.sw2.short_press','button.encoder.short_press','button.encoder.long_press','knob.rotate_ccw','knob.rotate_cw','joystick.up','joystick.down'];need(noNul(buttons)&&Array.isArray(buttons)&&buttons.length===8&&new Set(buttons.map(b=>b.event)).size===8&&new Set(buttons.map(b=>b.action)).size===8&&buttons.every(b=>physical.includes(b.event)&&Object.values(f.actions).includes(b.action)));}
 return true;
}
const cache=new WeakMap();let module;
function core(r){
 const signature=JSON.stringify([r.form,r.vars,r.pages]);let cached=cache.get(r);
 if(cached?.signature===signature)return cached;
 validateMixedForm(r);
 if(!module){const data=Uint8Array.from(atob(encodedFormCore),c=>c.charCodeAt(0));module=new WebAssembly.Module(data);if(WebAssembly.Module.imports(module).length)throw Error('Unexpected form imports');}
 const e=new WebAssembly.Instance(module,{}).exports,f=r.form,k=f.keyboard,names=Object.keys(r.vars),slot=n=>names.indexOf(n),string=s=>{const bytes=utf8.encode(s);if(bytes.length>63)throw Error('Form input exceeds bounded memory');new Uint8Array(e.memory.buffer,e.fv2_buffer(),128).set(bytes);};
 e.fv2_begin(f.fields.length,r.pages.findIndex(p=>p.id===f.page),slot(f.focus_var),slot(f.active_var),slot(k.index_var),slot(k.label_var),k.characters.length,f.fields.reduce((mask,q,i)=>mask|(q.kind==='text'?1<<i:0),0));
 f.fields.forEach((q,i)=>{e.fv2_field(i,slot(q.var),slot(q.draft_var),q.min??0,q.max??0,q.step??0,q.max_bytes??0);q.options?.forEach((o,j)=>{string(o.label);e.fv2_option(i,j,o.value);});});
 k.characters.forEach((c,i)=>{string(c);e.fv2_character(i);});string(k.space_label??'Space');e.fv2_space();if(!e.fv2_commit())throw Error('Native form configuration refused');
 const textAt=(address,capacity)=>{const bytes=new Uint8Array(e.memory.buffer,address,capacity),end=bytes.indexOf(0);if(end<0)throw Error('Invalid form text terminator');return decode.decode(bytes.subarray(0,end));};
 cached={signature,e,names,string,textAt,load(s){
  // Validate the entire typed snapshot before changing any core state.
  for(const n of names)if(!(r.vars[n].type==='int'?int(s.vars[n],-2147483648,2147483647):safeText(s.vars[n])))throw Error('Invalid typed form snapshot');
  e.fv2_clear_values();names.forEach((n,i)=>{const text=r.vars[n].type==='string';if(text)string(s.vars[n]);e.fv2_value(i,text?0:s.vars[n],text?1:0);});
 },store(s){const base=e.fv2_values(),view=new DataView(e.memory.buffer);names.forEach((n,i)=>{s.vars[n]=r.vars[n].type==='string'?textAt(base+i*68+4,64):view.getInt32(base+i*68,true);});}};
 cache.set(r,cached);return cached;
}
export function mixedForm(runtime,state,operation,argument){
 const c=core(runtime),e=c.e;c.load(state);const page=runtime.pages.findIndex(p=>p.id===state.page),op=OPS.indexOf(argument);
 if(operation==='allowed')return !!e.fv2_allowed(page,op);
 if(operation==='text'){const field=runtime.form.fields.findIndex(q=>q.id===argument);return c.textAt(e.fv2_display(field),121);}
 const before=JSON.stringify(state.vars);let result;
 if(operation==='initialize')result=e.fv2_initialize();else if(operation==='cancel')result=e.fv2_cancel();else if(operation==='apply')result=e.fv2_apply(page,op);
 else throw Error('Unknown bounded form operation');
 if(operation==='initialize'&&!result)throw Error('Unable to initialize typed form state');
 if(result){c.store(state);if(before!==JSON.stringify(state.vars))state.formEpoch=(state.formEpoch||0)+1;}return !!result;
}
export function replaceMixedDraft(runtime,state,field,text){
 if(!safeText(text))return 'invalid';const c=core(runtime);c.load(state);c.string(text);
 const result=c.e.fv2_replace(runtime.pages.findIndex(p=>p.id===state.page),runtime.form.fields.findIndex(q=>q.id===field));
 if(result===0){c.store(state);state.formEpoch=(state.formEpoch||0)+1;}
 return ['applied','unchanged','denied','invalid','limit'][result]??'invalid';
}
