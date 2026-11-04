import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {variableOwners,hostAction,programInputValues,presentedVariables,mixedHostSnapshot,hostDependent,hostGeometryReady,updateOwnedHostReply} from './variable-owners.js';
import {actionEnabled} from './input-policy.js';
import {initialApp} from './app-runtime.js';
import {validateMixedForm} from './form-v2-core.js';
import {smokeComponentProgram} from '../../skills/petui/scripts/component-program-smoke.mjs';
import {componentProgramConfig,createComponentProgram} from './component-program.js';
const suite=JSON.parse(fs.readFileSync(new URL('../../skills/petui/contracts/variable-owners-v1.json',import.meta.url),'utf8'));
test('shared writer plan/program rejection vectors remain data-only and bounded',()=>{
 const r=suite.runtime,p=variableOwners(r);assert.equal(p.mixed,true);assert.deepEqual([...p.host],['step']);componentProgramConfig(r);
 for(const patch of suite.reject){const bad=structuredClone(r),parts=patch.path.slice(1).split('/');let at=bad;for(const k of parts.slice(0,-1))at=at[k];at[parts.at(-1)]=patch.value;
  assert.throws(()=>variableOwners(bad),undefined,patch.path);assert.throws(()=>componentProgramConfig(bad),undefined,patch.path);
 }
 const old=structuredClone(r);delete old.ownership;assert.equal(variableOwners(old).mixed,false);assert.throws(()=>componentProgramConfig(old));
});
test('mixed program form keeps disjoint writers and CLI does not claim real host verification',()=>{
 const suite=JSON.parse(fs.readFileSync(new URL('../../skills/petui/contracts/component-program-form-v1.json',import.meta.url),'utf8'));
 const r=structuredClone(suite.runtime);r.host=true;r.host_context='p4-host-context-v1';r.ownership={engine:'p4-variable-owners-v1',host:['step']};
 assert(variableOwners(r).mixed);assert(validateMixedForm(r,suite.buttons));assert(componentProgramConfig(r));
 const result=smokeComponentProgram(r,suite.buttons);assert.equal(result.ok,true,JSON.stringify(result));
 assert.equal(result.syntheticHostInputs,true);assert.equal(result.hostServicesVerified,false);assert.equal(result.deviceVerified,false);
 const bad=structuredClone(r);bad.ownership.host=['titleDraft'];assert.throws(()=>validateMixedForm(bad,suite.buttons));
 const old=structuredClone(r);delete old.ownership;assert.throws(()=>validateMixedForm(old,suite.buttons));
});
test('host epochs retain away-back changes but ignore identical polls and unowned fields',()=>{
 const r=suite.runtime,a={enabled:true,variables:{step:2,count:4},ui:{mode:0,context:'a',actions:[]}};
 const first=updateOwnedHostReply(r,null,a),same=updateOwnedHostReply(r,first,{...a,variables:{step:2,count:900}});
 assert.equal(first._hostEpoch,1);assert.equal(same._hostEpoch,1);
 const changed=updateOwnedHostReply(r,same,{...a,variables:{step:5}}),back=updateOwnedHostReply(r,changed,a);
 assert.equal(changed._hostEpoch,2);assert.equal(back._hostEpoch,3);
 const exhausted=updateOwnedHostReply(r,{_hostEpoch:Number.MAX_SAFE_INTEGER},a);assert.equal(exhausted.enabled,false);
});
test('host service availability gates host actions and required inputs, not native actions or model',()=>{
 const r=structuredClone(suite.runtime);r.transitions.push({from:'*',on:'host.increment'});
 const native={...initialApp(r),vars:{count:17,step:2},program:{live:true,canEnqueue:true}};
 const reply={fresh:true,variables:{count:900,step:5},values:{title:'host'},ui:{mode:0,context:'epoch-a',actions:['host.increment']}};
 const view=mixedHostSnapshot(r,native,reply);
 assert.equal(view.vars.count,17);assert.equal(view.vars.step,5);assert.equal(native.vars.step,2);
 assert.equal(actionEnabled(r,view,'add'),true);assert.equal(actionEnabled(r,view,'subtract'),true);
 assert.equal(actionEnabled(r,view,'host.increment'),true);
 const missing=mixedHostSnapshot(r,native,{fresh:false,variables:reply.variables});
 assert.equal(actionEnabled(r,missing,'add'),false);assert.equal(actionEnabled(r,missing,'subtract'),true);
 assert.equal(actionEnabled(r,missing,'host.increment'),false);
 assert.equal(actionEnabled(r,mixedHostSnapshot(r,native,{fresh:true,variables:{step:5}}),'host.increment'),false);
 assert.equal(hostDependent(r,'subtract'),false);assert.equal(hostDependent(r,'add'),true);
 assert.equal(hostDependent(r,'subtract',{show:{var:'step'}}),true);
 assert.equal(hostDependent(r,'subtract',{show:{var:'count'}}),false);
 assert.equal(hostGeometryReady(r,missing,'subtract',{show:{var:'step'}}),false);
 assert.equal(hostGeometryReady(r,missing,'subtract',{show:{var:'count'}}),true);
 assert.equal(hostGeometryReady(r,view,'subtract',{show:{var:'step'}}),true);
 assert.equal(mixedHostSnapshot({...r,ownership:undefined},native,reply),native);
 assert.equal(hostAction({...r,ownership:undefined},'subtract'),true);
});
test('one immutable host input never writes native/form model and stale host does not block pure native input',()=>{
 const r=suite.runtime,native={count:7,step:2};
 assert.equal(hostAction(r,'host.increment'),true);assert.equal(hostAction(r,'add'),false);
 assert.equal(programInputValues(r,native,'add',{fresh:false,variables:{step:5}}),null);
 assert.deepEqual(programInputValues(r,native,'subtract',{fresh:false}),native);
 const reply={fresh:true,variables:{step:5,count:999}},captured=programInputValues(r,native,'add',reply);
 assert.deepEqual(captured,{count:7,step:5});reply.variables.step=9;assert.equal(captured.step,5);assert.equal(native.step,2);
 assert.deepEqual(presentedVariables(r,native,reply),{count:7,step:9});
 assert.deepEqual(presentedVariables(r,native,{fresh:false,variables:{count:999,step:9}}),{count:7,step:0});
 const core=createComponentProgram(r);assert.equal(core.enqueue('add',captured),'ok');
 let next=null;for(let i=0;i<10&&!next;i++)next=core.pump(native,0);
 assert.equal(next.count,5);assert.equal(native.count,7);assert.equal(next.step,2);core.cancel();
});
