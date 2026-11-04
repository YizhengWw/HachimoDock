import test from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import {readFileSync} from 'node:fs';
const source=readFileSync(new URL('../public/startup-guard.js',import.meta.url),'utf8');
function setup(){
 const elements=[],events={},timers=[],logs=[];let reloads=0,consent=true;
 function node(tag){return {tagName:tag.toUpperCase(),style:{},children:[],listeners:{},setAttribute(){},appendChild(n){this.children.push(n);elements.push(n);},replaceChildren(){this.children=[];},addEventListener(k,v){this.listeners[k]=v;},remove(){this.removed=true;}};}
 const document={body:node('body'),createElement:node,getElementById:id=>elements.find(n=>n.id===id&&!n.removed)};
 const loading=node('section');loading.id='startup-loading';elements.push(loading);
 const window={addEventListener:(name,fn)=>{events[name]=fn;},setTimeout:fn=>timers.push(fn),confirm:()=>consent,location:{reload:()=>reloads++}};
 vm.runInNewContext(source,{window,document,console:{error:(...v)=>logs.push(v)}});
 return {window,document,events,timers,logs,loading,get reloads(){return reloads;},set consent(v){consent=v;}};
}
test('entry script failure is visible without React and reload is manual, single and mutation-free',()=>{
 const env=setup();env.events.error({target:{tagName:'SCRIPT',src:'private-path?token=secret'}});
 const panel=env.document.getElementById('startup-recovery');assert.ok(panel);
 const button=panel.children.find(n=>n.tagName==='BUTTON');assert.equal(env.reloads,0);
 env.consent=false;button.listeners.click();assert.equal(env.reloads,0);
 env.consent=true;button.listeners.click();button.listeners.click();assert.equal(env.reloads,1);
 assert.ok(!JSON.stringify(env.logs).includes('secret'));
});
test('first JS category is retained without recording sensitive exception text',()=>{
 const env=setup();env.events.error({target:env.window,error:{name:'ReferenceError',message:'PRIVATE CONTENT'}});
 env.events.unhandledrejection({reason:{name:'TypeError',message:'SECRET'}});
 assert.equal(env.logs[0][2],'ReferenceError');assert.equal(env.logs[1][2],'ReferenceError');
 assert.ok(!JSON.stringify(env.logs).includes('PRIVATE'));
});
test('a missing app mount times out visibly; successful mount suppresses timeout',()=>{
 const stalled=setup();stalled.timers[0]();assert.ok(stalled.document.getElementById('startup-recovery'));
 const ready=setup();ready.window.__PET_STARTUP__.ready();ready.timers[0]();
 assert.equal(ready.loading.removed,true);assert.equal(ready.document.getElementById('startup-recovery'),undefined);
});
