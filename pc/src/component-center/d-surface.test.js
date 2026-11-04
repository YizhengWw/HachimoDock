import test from 'node:test';import assert from 'node:assert/strict';import fs from 'node:fs';
import {dSurface,dAction,dSnapshot,flowLines,visibleRows} from './d-surface.js';
import {initialApp,appAction} from './app-runtime.js';
import {formAllowed,formText} from './app-form.js';
const suite=name=>JSON.parse(fs.readFileSync(new URL(`../../skills/petui/contracts/${name}.json`,import.meta.url)));
test('real runtime initializes and executes the device v3 form core',()=>{
 const r=suite('app-form-v3').runtime;let s=initialApp(r);assert.equal(s.vars.editing,0);assert.equal(formAllowed(r,s,'accept'),true);
 s=appAction(r,s,r.form.actions.accept);assert.equal(s.vars.editing,1);assert.equal(formText(r,s,'name'),'中');
 s=appAction(r,s,r.form.actions.cancel);assert.equal(s.vars.editing,0);assert.equal(s.vars.name,'中');
});
test('longtext matches shared C/Python line vectors',()=>{for(const c of suite('longtext-v1').flow)assert.deepEqual(flowLines(c.text,c.width,c.font_size,c.max_lines??256),c.lines);});
test('shell page 4 is private while omitted page is common, including 0 and 11',()=>{
 const runtime={pages:Array.from({length:12},(_,i)=>({id:`p${i}`})),surface:{shell:'p4-app-shell-v1',elements:[0,4,11,undefined].map(page=>({type:'text',...(page===undefined?{}:{page}),text:String(page)}))}};
 for(const page of [0,4,11]){const out=dSurface(runtime,{page:`p${page}`,vars:{}});assert.deepEqual(out.elements.filter(n=>!n.hidden).map(n=>n.text),[String(page),'undefined']);}
});
test('tabs select actual declared root pages and clear history',()=>{const r=suite('app-shell-v1').runtime,s=initialApp(r),t=r.surface.tabs;const next=dAction(r,s,'tab.next',{_tabIndex:t.items.length-1});assert.equal(next.page,r.pages[t.items.at(-1).page].id);assert.deepEqual(next.history,[]);assert.equal(s.vars[t.var],0);});
test('rows use filtered selection and longtext publishes page count',()=>{
 const runtime={data:{filters:[{var:'filter',field:'meta',values:['a']}],select_var:'sel'},surface:{elements:[{type:'text',row_bind:'label'}]}};
 const snapshot={vars:{filter:1,sel:4},data:{rows:[{id:'1',meta:'a',label:'one'},{id:'2',meta:'b',label:'two'}]}};
 assert.equal(visibleRows(runtime,snapshot).length,1);assert.equal(dSurface(runtime,snapshot).elements[0].text,'one');
 const r=suite('longtext-v1').runtime;assert.ok(dSnapshot(r,initialApp(r)).vars);
});
test('v3 form keeps 32-variable profile and unrelated 127-byte strings intact',()=>{
 const r=structuredClone(suite('app-form-v3').runtime);r.quota='p4-quota-v3';
 for(let i=Object.keys(r.vars).length;i<32;i++)r.vars[`extra${i}`]={type:'string',init:'x'.repeat(127)};
 let s=initialApp(r);s=appAction(r,s,r.form.actions.accept);s=appAction(r,s,r.form.actions.cancel);
 assert.equal(Object.keys(s.vars).length,32);for(const [k,v] of Object.entries(r.vars))if(k.startsWith('extra'))assert.equal(s.vars[k],v.init);
});
