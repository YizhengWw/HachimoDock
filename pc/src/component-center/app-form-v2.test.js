import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {initialApp,appAction,appNodeValue} from './app-runtime.js';
import {formInitialize,formCancel,formAllowed,formApply,formContext,formStructuralDisabled} from './app-form.js';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {validateMixedForm,replaceMixedDraft} from './form-v2-core.js';
const suite=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/app-form-v2.json',import.meta.url))),r=suite.runtime;
test('mixed form uses native C/WASM for shared typed event vectors',()=>{
 assert(validateMixedForm(r,suite.buttons));
 for(const c of suite.cases){let s=initialApp(r);Object.assign(s.vars,c.restore);formInitialize(r,s);for(const a of c.actions)s=a==='@hide'?formCancel(r,s):appAction(r,s,a);
  assert.deepEqual(Object.values(s.vars),c.vars,c.name);assert.equal(s.page,c.page,c.name);assert.deepEqual(r.surface.elements.slice(0,2).map(n=>appNodeValue(n,s,{},r)),c.text,c.name);
 }
});
test('mixed form strict rejection matches all native declaration vectors',()=>{
 for(const c of suite.invalid){const bad=structuredClone(r);let p=bad;for(const k of c.path.slice(0,-1))p=p[k];p[c.path.at(-1)]=c.value;assert.throws(()=>validateMixedForm(bad,suite.buttons),JSON.stringify(c));}
 const bad=structuredClone(r);bad.form.keyboard.characters=['\ud800'];assert.throws(()=>validateMixedForm(bad));
 assert.throws(()=>validateMixedForm(r,suite.buttons.slice(1)));
});
test('trusted IME replacement edits draft only and never truncates input',()=>{
 const s=initialApp(r);assert.equal(replaceMixedDraft(r,s,'name','abc'),'denied');formApply(r,s,'accept');const before=formContext(r,s);
 assert.equal(replaceMixedDraft(r,s,'name','用户😀'),'applied');assert.equal(s.vars.title,'中');assert.equal(s.vars.titleDraft,'用户😀');assert.notEqual(formContext(r,s),before);
 for(const [text,result] of [['用户😀','unchanged'],['12345678901','limit'],['\udfff','invalid'],['a\0b','invalid']])assert.equal(replaceMixedDraft(r,s,'name',text),result);
 assert.equal(s.vars.titleDraft,'用户😀');assert.equal(replaceMixedDraft(r,s,'level','abc'),'denied');formApply(r,s,'accept');assert.equal(s.vars.title,'用户😀');
});
test('invalid typed snapshots never partially mutate the preview or committed values',()=>{
 const s=initialApp(r);s.vars.amountDraft=NaN;const before=structuredClone(s);assert.throws(()=>formApply(r,s,'accept'));assert.deepEqual(s,before);
 s.vars.amountDraft=2;s.vars.titleDraft='\ud800';const again=structuredClone(s);assert.throws(()=>formAllowed(r,s,'accept'));assert.deepEqual(s,again);
});
test('product form WASM has fixed memory and zero imports',()=>{
 const module=new WebAssembly.Module(readFileSync(new URL('./form-v2-core.wasm',import.meta.url)));assert.deepEqual(WebAssembly.Module.imports(module),[]);
 const e=new WebAssembly.Instance(module,{}).exports;assert.equal(e.memory.buffer.byteLength,262144);assert.throws(()=>e.memory.grow(1));
});
test('one-field and one-character form smoke proves disabled structure instead of requiring impossible changes',()=>{
 const one=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/component-program-form-v1.json',import.meta.url)));
 delete one.runtime.program;one.runtime.form.keyboard.characters=['a'];
 one.runtime.surface.elements=one.runtime.surface.elements.filter(n=>!['add','subtract'].includes(n.action));
 assert(validateMixedForm(one.runtime,one.buttons));
 const disabled={'field.prev':'single_field','field.next':'single_field','value.down':'single_character_keyboard','value.up':'single_character_keyboard'};
 assert.deepEqual(formStructuralDisabled(one.runtime),disabled);
 let state=initialApp(one.runtime);
 for(const a of ['form.accept','text.append','text.erase','leave','back','form.cancel']){
  state=appAction(one.runtime,state,a);
  for(const operation of ['previous','next','decrease','increase'])assert.equal(formAllowed(one.runtime,state,operation),false);
 }
 const invoke=runtime=>{const p=spawnSync(process.execPath,[fileURLToPath(new URL('../../skills/petui/scripts/form-v2-host.mjs',import.meta.url))],{input:JSON.stringify({mode:'smoke',runtime,buttons:one.buttons}),encoding:'utf8'});assert.equal(p.status,0,p.stderr);return JSON.parse(p.stdout);};
 const report=invoke(one.runtime);assert(report.ok,JSON.stringify(report));assert.deepEqual(report.disabledActions,disabled);
 assert(!report.visibleActions.includes('value.up'));assert(report.visibleActions.includes('text.append'));
 const dead=structuredClone(one.runtime);dead.transitions.push({from:'*',from_page:'home',on:'dead'});dead.surface.elements.push({type:'button',page:1,x:20,y:300,w:180,h:48,color:65535,text:'Dead',action:'dead'});
 const failed=invoke(dead);assert.equal(failed.ok,false);assert(!Object.hasOwn(failed.disabledActions,'dead'));
 assert.deepEqual(formStructuralDisabled({...one.runtime,form:{...one.runtime.form,engine:'unknown'}}),{});
});
