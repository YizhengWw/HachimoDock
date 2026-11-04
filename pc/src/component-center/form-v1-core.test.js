import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {initialApp} from './app-runtime.js';
import {formApply,formAllowed,formCancel,formInitialize,formText} from './app-form.js';
import {legacyForm} from './form-v1-core.js';
const suite=JSON.parse(fs.readFileSync(new URL('../../skills/petui/contracts/app-form-v1.json',import.meta.url)));
const fresh=()=>structuredClone(suite.runtime);
test('legacy editor ships the same fixed native core with no host imports or memory growth',()=>{
 const wasm=fs.readFileSync(new URL('./form-v1-core.wasm',import.meta.url)),m=new WebAssembly.Module(wasm);
 assert.deepEqual(WebAssembly.Module.imports(m),[]);const e=new WebAssembly.Instance(m,{}).exports;
 assert.equal(e.memory.buffer.byteLength,65536);assert.throws(()=>e.memory.grow(1),RangeError);
 assert(wasm.equals(fs.readFileSync(new URL('../../skills/petui/scripts/form-v1-core.wasm',import.meta.url))));
 assert.equal(fs.readFileSync(new URL('./form-v1-core.js',import.meta.url),'utf8'),fs.readFileSync(new URL('../../skills/petui/scripts/form-v1-core.mjs',import.meta.url),'utf8'));
});
test('the complete typed snapshot is validated before any modeled field is changed',()=>{
 const r=fresh(),s=initialApp(r);formApply(r,s,'accept');
 for(const bad of [NaN,Infinity,true,1.5,2147483648]){
  const copy=structuredClone(s);copy.vars.amountDraft=bad;const before=structuredClone(copy);
  assert.throws(()=>formApply(r,copy,'accept'));assert.deepEqual(copy,before);
 }
 const bad=structuredClone(s);delete bad.vars.choiceDraft;assert.throws(()=>formCancel(r,bad));assert.equal(bad.vars.editing,1);
});
test('one cached interpreter cannot mix separate snapshots, runtime declarations or unrelated string variables',()=>{
 const r=fresh();r.vars.unrelated={type:'string',init:'keep'};
 const a=initialApp(r),b=initialApp(r);b.vars.choice=30;formInitialize(r,b);
 formApply(r,a,'accept');formApply(r,a,'increase');assert.equal(a.vars.choiceDraft,20);
 assert.equal(formText(r,b,'mode'),'Bright');assert.equal(formAllowed(r,b,'increase'),false);
 assert.equal(formText(r,a,'mode'),'Balanced');assert.equal(a.vars.unrelated,'keep');
 formApply(r,a,'accept');assert.equal(a.vars.choice,20);assert.equal(b.vars.choice,30);
 const other=fresh(),c=initialApp(other);formApply(other,c,'accept');formApply(other,c,'increase');formCancel(other,c);
 assert.equal(c.vars.choice,10);assert.equal(formText(r,a,'mode'),'Balanced');assert.equal(a.vars.unrelated,'keep');
});
test('invalid edited metadata never falls back to a cached declaration or partially changes state',()=>{
 const r=fresh(),s=initialApp(r);formApply(r,s,'accept');const before=structuredClone(s),q=r.form.fields[1];
 const step=q.step;q.step=0;assert.throws(()=>formApply(r,s,'increase'));assert.deepEqual(s,before);
 q.step=step;assert.equal(formApply(r,s,'increase'),true);assert.equal(s.vars.choiceDraft,20);
 for(const mutate of [r=>r.form.focus_var=r.form.active_var,r=>r.form.fields[1].draft_var=r.form.fields[0].var,r=>r.form.fields[0].options[0].value=true,r=>r.form.fields[0].options[1].value=r.form.fields[0].options[0].value,r=>r.form.fields[0].options=Array(2),r=>r.form.fields[1].max=2147483648]){
  const invalid=fresh();mutate(invalid);assert.throws(()=>initialApp(invalid));
 }
});
test('int64 intermediate arithmetic and native choice fallback retain the legacy draft-only contract',()=>{
 const r=fresh(),q=r.form.fields[1];q.min=-1e9;q.max=1e9;q.step=1e9;r.limits??={};r.limits.amount=[-1e9,1e9];r.limits.amountDraft=[-1e9,1e9];
 r.vars.amount.init=r.vars.amountDraft.init=-1e9;const s=initialApp(r);formApply(r,s,'next');formApply(r,s,'accept');
 formApply(r,s,'increase');assert.equal(s.vars.amount,-1e9);assert.equal(s.vars.amountDraft,0);
 formApply(r,s,'increase');assert.equal(s.vars.amountDraft,1e9);assert.equal(formApply(r,s,'increase'),false);
 formApply(r,s,'accept');assert.equal(s.vars.amount,1e9);
 s.vars.choice=99;formInitialize(r,s);assert.equal(formText(r,s,'mode'),'—');formApply(r,s,'accept');assert.equal(s.vars.choice,99);assert.equal(s.vars.choiceDraft,10);
});
test('legacy raw ABI rejects narrowed indexes, missing options and aliased field roles',()=>{
 const m=new WebAssembly.Module(fs.readFileSync(new URL('./form-v1-core.wasm',import.meta.url)));
 const create=()=>new WebAssembly.Instance(m,{}).exports;
 let e=create();e.f1_begin(65537,0,2,3);assert.equal(e.f1_commit(),0);assert.equal(e.f1_apply(0,4),0);
 e=create();e.f1_begin(1,0,2,3);e.f1_field(0,0,65537,0,10,1,0);assert.equal(e.f1_commit(),0);
 e=create();e.f1_begin(1,0,2,3);e.f1_field(0,0,1,0,0,0,2);e.f1_option(0,1,10);assert.equal(e.f1_commit(),0);
 e=create();e.f1_begin(1,0,2,3);e.f1_field(0,2,1,0,10,1,0);assert.equal(e.f1_commit(),0);
 assert.equal(legacyForm({}, {vars:{}},'apply','accept'),false);
});
