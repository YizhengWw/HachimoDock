import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {initialApp,appAction,appNodeValue} from './app-runtime.js';
import {formInitialize,formCancel,formContext,formOperation} from './app-form.js';
import {actionEnabled} from './input-policy.js';
import * as mirror from '../../skills/petui/scripts/app-runtime.mjs';
const suite=JSON.parse(fs.readFileSync(new URL('../../skills/petui/contracts/app-form-v1.json',import.meta.url)));
for(const c of suite.cases)test(`form: ${c.name}`,()=>{
  const r=suite.runtime;let s=initialApp(r),other=mirror.initialApp(r);
  for(const state of [s,other]){Object.assign(state.vars,c.restore);formInitialize(r,state);}
  for(const a of c.actions){s=a==='@hide'?formCancel(r,s):appAction(r,s,a);other=a==='@hide'?formCancel(r,other):mirror.appAction(r,other,a);assert.deepEqual(s,other);}
  assert.deepEqual(Object.values(s.vars),c.vars);assert.equal(s.page,c.page);
  assert.deepEqual(r.surface.elements.slice(0,2).map(n=>appNodeValue(n,s,{},r)),c.text);
});
test('form pointer identity survives same-frame away/back; directional action remains available',()=>{
  const r=suite.runtime;let s=appAction(r,initialApp(r),'form.accept');const before=formContext(r,s);
  s=appAction(r,appAction(r,s,'value.up'),'value.down');
  assert.equal(s.vars.choiceDraft,10);assert.notEqual(formContext(r,s),before);
  assert.equal(actionEnabled(r,s,'value.up'),true);
});
test('form operations come from actual bindings, not SW1/SW2 identities',()=>{
  const r=suite.runtime,bindings=structuredClone(suite.buttons);[bindings[4].event,bindings[5].event]=[bindings[5].event,bindings[4].event];
  const event=name=>bindings.find(b=>b.event===name).action;
  let s=appAction(r,initialApp(r),event('button.sw2.short_press'));
  assert.equal(s.vars.editing,1);s=appAction(r,s,event('knob.rotate_cw'));assert.equal(s.vars.choiceDraft,20);
  s=appAction(r,s,event('button.sw1.short_press'));assert.equal(s.vars.editing,0);assert.equal(s.vars.choice,10);
  assert.equal(bindings.filter(b=>formOperation(r,b.action)).length,6);
});
test('form source mirrors exactly; UI exposes physical controls and discards hidden drafts',()=>{
  const source=fs.readFileSync(new URL('./app-form.js',import.meta.url),'utf8');
  // The mechanical mirror changes only the module suffix, as with the other
  // shared modules; every executable body must remain byte-for-byte identical.
  assert.equal(source.replaceAll('./form-v1-core.js','./form-v1-core.mjs').replaceAll('./form-v2-core.js','./form-v2-core.mjs').replaceAll('./form-v3-core.js','./form-v3-core.mjs'),fs.readFileSync(new URL('../../skills/petui/scripts/app-form.mjs',import.meta.url),'utf8'));
  const ui=fs.readFileSync(new URL('./AppSurface.jsx',import.meta.url),'utf8');
  assert.ok(ui.includes('表单实体按键预览'));assert.ok(ui.includes('p.editingContext!==editingContext'));
  assert.ok(ui.includes('if(document.hidden)discard()'));
});
