// Local stdin/stdout validation adapter. No HTTP, credentials, storage or IPC.
import {readFileSync} from 'node:fs';
import {initialApp,appAction,appAdvance,appVisible,appNodeValue} from './app-runtime.mjs';
import {mixedForm,validateMixedForm} from './form-v2-core.mjs';
import {formStructuralDisabled} from './app-form.mjs';
const input=JSON.parse(readFileSync(0,'utf8')),r=input.runtime;
validateMixedForm(r,input.buttons);
if(input.mode==='operation'){
 const state=input.state,result=mixedForm(r,state,input.operation,input.argument);
 console.log(JSON.stringify({state,result}));
}else if(input.mode==='smoke'){
 const disabled=formStructuralDisabled(r);
 const declared=[...new Set([...input.buttons.map(b=>b.action),...r.surface.elements.filter(n=>n.action).map(n=>n.action),...Object.values(r.input?.swipe||{})])];
 const hostActions=r.ownership?declared.filter(a=>a.startsWith('host.')):[],actions=declared.filter(a=>!hostActions.includes(a));
 const required=actions.filter(a=>!Object.hasOwn(disabled,a)),seen=new Set(),effective=new Set(),states=new Set(),queue=[[initialApp(r),0]];
 let cursor=0,truncated=false,ticks=false;
 const identity=s=>JSON.stringify([s.state,s.page,s.history||[],s.vars]);
 const visible=s=>JSON.stringify(r.surface.elements.filter(n=>appVisible(n,r.pages.findIndex(p=>p.id===s.page),s.vars,r.states.indexOf(s.state))).map(n=>[n.type,appNodeValue(n,s,{},r)]));
 while(cursor<queue.length&&seen.size<512){
  const [s,depth]=queue[cursor++],key=identity(s);if(seen.has(key))continue;seen.add(key);states.add(s.state);
  for(const a of actions){const next=appAction(r,s,a);if(visible(s)!==visible(next))effective.add(a);if(depth<32&&!seen.has(identity(next)))queue.push([next,depth+1]);}
  if((r.tick||[]).length){const next=appAdvance(r,s,1000);ticks||=visible(s)!==visible(next);if(depth<32&&!seen.has(identity(next)))queue.push([next,depth+1]);}
  if(required.every(a=>effective.has(a))&&r.states.every(s=>states.has(s))&&(!(r.tick||[]).length||ticks))break;
 }
 truncated=cursor<queue.length&&seen.size>=512;
 const errors=[];if(required.some(a=>!effective.has(a)))errors.push('Some form actions have no reachable visible feedback');if(r.states.some(s=>!states.has(s)))errors.push('Some declared states are unreachable');
 if((r.tick||[]).length&&!ticks)errors.push('Tick visibility requires a separate bounded timing scenario');
 console.log(JSON.stringify({ok:!errors.length,mode:'shared-native-mixed-form',visibleActions:[...effective].sort(),disabledActions:disabled,reachableStates:[...states].sort(),explorationStates:seen.size,explorationTruncated:truncated,ticksAdvanced:ticks,errors,deviceVerified:false,hostServicesVerified:false,...(r.ownership?{syntheticHostInputs:true,hostActionsContractOnly:hostActions}:{})}));
}else throw Error('Unknown local form verification mode');
