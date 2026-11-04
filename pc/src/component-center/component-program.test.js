import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {componentProgramConfig,createComponentProgram} from './component-program.js';
import {smokeComponentProgram} from '../../skills/petui/scripts/component-program-smoke.mjs';
import {actionEnabled,actionAccepted} from './input-policy.js';
import {initialApp,appAction,appAdvance} from './app-runtime.js';
import {validateMixedForm} from './form-v2-core.js';
const suite=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/component-program-v1.json',import.meta.url)));
function patched(r,p){const out=structuredClone(r),keys=p.path.slice(1).split('/');let o=out;for(const k of keys.slice(0,-1))o=o[k];o[keys.at(-1)]=p.value;return out;}
test('component program shares strict schema and sole-writer contract with native and authoring',()=>{
  assert(componentProgramConfig(suite.runtime));for(const p of suite.reject)assert.throws(()=>componentProgramConfig(patched(suite.runtime,p)),p.path);
  const r=structuredClone(suite.runtime);delete r.program;assert.equal(componentProgramConfig(r),null);
  for(const key of ['focus_var','active_var'])assert.throws(()=>componentProgramConfig({...suite.runtime,form:{[key]:'count'}}));
  for(const key of ['var','draft_var'])assert.throws(()=>componentProgramConfig({...suite.runtime,form:{fields:[{[key]:'count'}]}}));
  for(const key of ['index_var','label_var'])assert.throws(()=>componentProgramConfig({...suite.runtime,form:{keyboard:{[key]:'count'}}}));
  assert.throws(()=>componentProgramConfig({...suite.runtime,form:{actions:{accept:'add'}}}));
  const feedback=structuredClone(suite.runtime);feedback.program.outputs[1]='step';assert(componentProgramConfig(feedback));
});
test('authoring smoke uses the same C adapter and proves real visible inputs without granting device execution',()=>{
  const before=JSON.stringify(suite),report=smokeComponentProgram(suite.runtime,suite.buttons);
  assert(report.ok);assert.equal(report.mode,'shared-native-component-program');assert.deepEqual(report.visibleActions,['add','subtract']);
  assert.equal(report.deviceVerified,false);assert.equal(report.hostServicesVerified,false);assert.equal(JSON.stringify(suite),before);
  const timer=structuredClone(suite.runtime);timer.program.timers=[{id:1,delayMs:200,intervalMs:400,words:[1]}];assert(smokeComponentProgram(timer,suite.buttons).timersAdvanced);
  const invalid=structuredClone(suite.runtime);invalid.program.actions.add=[200];const failed=smokeComponentProgram(invalid,suite.buttons);assert.equal(failed.ok,false);assert(failed.errors.some(e=>e.includes('stopped')));
});
test('component adapter is a reproducible mechanical mirror with no alternate interpreter',()=>{
  const source=readFileSync(new URL('./component-program.js',import.meta.url),'utf8').replaceAll('./program-core.js','./program-core.mjs').replaceAll('./variable-owners.js','./variable-owners.mjs');
  assert.equal(source,readFileSync(new URL('../../skills/petui/scripts/component-program.mjs',import.meta.url),'utf8'));
});
function drain(p,base,now=0){for(let i=0;i<100;i++){const next=p.pump(base,now);if(next)return next;if(!p.diagnostic().live)return base;}throw Error('fixture did not finish');}
test('program cooldown begins at actual acceptance, rejects busy attempts and permits the exact boundary',()=>{
 const r=structuredClone(suite.runtime);r.input.cooldown={add:250};
 const core=createComponentProgram(r);let s={...initialApp(r),program:core.diagnostic()};
 assert.equal(actionAccepted(r,s,'add',false),s);assert.equal(actionAccepted(r,s,'add','queue_full'),s);
 assert(actionEnabled(r,s,'add'));assert.equal(core.enqueue('add',s.vars),'ok');s=actionAccepted(r,s,'add');
 assert.equal(s.actionTimes.add,0);assert.equal(actionEnabled(r,s,'add'),false);
 s.vars=drain(core,s.vars);s=appAdvance(r,s,249);assert.equal(actionEnabled(r,s,'add'),false);
 s=appAdvance(r,s,1);assert.equal(actionEnabled(r,s,'add'),true);
 assert.equal(core.enqueue('add',s.vars),'ok');s=actionAccepted(r,s,'add');s.vars=drain(core,s.vars,250);assert.equal(s.vars.count,4);
 const backwards={...s,clock:249};assert.equal(actionEnabled(r,backwards,'add'),false);core.cancel();
 r.surface.elements[0].show={var:'count',min:4,max:100};
 const smoke=smokeComponentProgram(r,suite.buttons);assert(smoke.ok,JSON.stringify(smoke));
 assert.deepEqual(smoke.visibleActions,['add','subtract']);
});
test('actual C component preview captures inputs and commits one complete projected model',()=>{
  const p=createComponentProgram(suite.runtime);let base={count:0,step:2,unbound:'kept'};
  assert.equal(p.enqueue('add',base),'ok');base.step=3;assert.equal(p.pump(base,0),null);assert.equal(p.snapshot().outputs[0],0);
  base=drain(p,base);assert.deepEqual(base,{count:2,step:3,unbound:'kept'});
  assert.equal(p.enqueue('subtract',base),'ok');base=drain(p,base);assert.equal(base.count,1);assert.equal(p.snapshot().values[0],1);
  assert.equal(p.diagnostic().lastReport.result,'committed');assert.equal(p.diagnostic().sequence,2);
});
test('trap and manual stop preserve committed cells and cannot resume on another pump',()=>{
  const p=createComponentProgram(suite.runtime);let base={count:0,step:2};p.enqueue('add',base);base=drain(p,base);
  p.enqueue('add',{...base,step:200});assert.equal(drain(p,base).count,2);assert.equal(p.diagnostic().lastReport.result,'bounds');
  assert.equal(p.diagnostic().live,false);assert.equal(p.snapshot().values[0],2);assert.equal(p.pump(base,100000),null);assert.equal(p.enqueue('subtract',base),'stopped');
  const resumed=createComponentProgram(suite.runtime,{seed:p.snapshot()});resumed.enqueue('subtract',base);base=drain(resumed,base);assert.equal(base.count,1);
  resumed.cancel();assert.equal(resumed.pump(base,200000),null);assert.equal(resumed.snapshot().values[0],1);
});
test('same-C monotonic timers coalesce accepted work and do not revive a cancelled owner',()=>{
  const runtime=structuredClone(suite.runtime);runtime.program.timers=[{id:1,delayMs:200,intervalMs:400,words:[1]}];
  const p=createComponentProgram(runtime,{nowMs:500});let base={count:0,step:2};assert.equal(p.pump(base,699),null);assert.equal(p.diagnostic().sequence,0);
  base=drain(p,base,700);assert.equal(base.count,1);assert.equal(p.diagnostic().sequence,1);
  assert.equal(p.pump(base,699),null);assert.equal(p.diagnostic().live,false);assert.equal(p.pump(base,999999),null);assert.equal(p.snapshot().outputs[0],1);
});
test('incompatible seed and output contracts reject before replacing the old private state',()=>{
  const p=createComponentProgram(suite.runtime),seed=p.snapshot();
  assert.throws(()=>createComponentProgram(suite.runtime,{seed:{...seed,schema:2}}));
  const r=structuredClone(suite.runtime);r.limits.count=[1,100];assert.throws(()=>createComponentProgram(r,{seed}));
  assert.deepEqual(p.snapshot(),seed);p.cancel();
});
test('full native queue disables new UI input without discarding or replaying accepted events',()=>{
  const p=createComponentProgram(suite.runtime);let base={count:0,step:2};
  for(let i=0;i<4;i++)assert.equal(p.enqueue('add',base),'ok');
  assert.equal(p.diagnostic().queued,4);assert.equal(p.diagnostic().canEnqueue,false);
  assert.equal(actionEnabled(suite.runtime,{program:p.diagnostic()},'add'),false);
  assert.equal(p.enqueue('add',base),'queue_full');assert.equal(p.diagnostic().sequence,4);
  p.pump(base,0);assert.equal(p.diagnostic().queued,3);assert.equal(p.diagnostic().canEnqueue,true);
  assert.equal(p.enqueue('add',base),'ok');assert.equal(p.diagnostic().sequence,5);
  for(let i=0;i<40&&p.snapshot().revision<5;i++){const next=p.pump(base,0);if(next)base=next;}
  assert.equal(base.count,10);assert.equal(p.snapshot().revision,5);assert.equal(p.diagnostic().queued,0);
  p.cancel();assert.equal(p.diagnostic().canEnqueue,false);
});
test('program, text draft, timer and navigation have disjoint writers in one real native preview',()=>{
  const mixed=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/component-program-form-v1.json',import.meta.url)));
  const r=mixed.runtime;assert(validateMixedForm(r,mixed.buttons));assert(componentProgramConfig(r));
  const p=createComponentProgram(r);let state=appAction(r,initialApp(r),'form.accept');
  state=appAction(r,state,'text.append');assert.equal(state.vars.titleDraft,'中a');assert.equal(state.vars.editing,1);
  assert.equal(p.enqueue('add',state.vars),'ok');state.vars=drain(p,state.vars);assert.equal(state.vars.count,2);
  assert.equal(state.vars.titleDraft,'中a');assert.equal(state.vars.title,'中');assert.equal(state.vars.editing,1);
  state.vars=drain(p,state.vars,200);assert.equal(state.vars.count,3);assert.equal(state.vars.titleDraft,'中a');
  state=appAction(r,state,'form.accept');assert.equal(state.vars.title,'中a');assert.equal(state.vars.count,3);
  state=appAction(r,appAction(r,state,'form.accept'),'text.erase');assert.equal(state.vars.titleDraft,'中');
  state=appAction(r,state,'leave');assert.equal(state.page,'home');assert.equal(state.vars.editing,0);assert.equal(state.vars.titleDraft,'中a');
  state=appAction(r,state,'back');assert.equal(state.page,'edit');assert.equal(state.vars.count,3);
  const seed=p.snapshot();p.cancel();const reopened=createComponentProgram(r,{seed,nowMs:200});assert.equal(reopened.snapshot().outputs[0],3);reopened.cancel();
  const report=smokeComponentProgram(r,mixed.buttons);assert(report.ok,JSON.stringify(report));
  assert.deepEqual(report.disabledActions,{'field.prev':'single_field','field.next':'single_field'});
  assert(report.visibleActions.includes('add')&&report.visibleActions.includes('leave')&&report.visibleActions.includes('back'));
});
