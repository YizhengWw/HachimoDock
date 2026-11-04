// Local, bounded replay over the product C VM. No provider/device/save access.
// Synchronous draining is for this CLI only, never the browser/device scheduler.
import {createComponentProgram} from './component-program.mjs';
import {initialApp,appAction,appAdvance,appVisible,appNodeValue,appDashboard,appPage} from './app-runtime.mjs';
import {actionEnabled,actionAccepted} from './input-policy.mjs';
import {projectNode} from './app-viewport.mjs';
import {formStructuralDisabled} from './app-form.mjs';
import {hostAction,programInputValues,mixedHostSnapshot} from './variable-owners.mjs';

export function smokeComponentProgram(runtime,buttons){
 const actions=[...new Set([...buttons.map(b=>b.action),...runtime.surface.elements.filter(n=>n.action).map(n=>n.action),...Object.values(runtime.input?.swipe||{})])];
 const hostActions=runtime.ownership?actions.filter(a=>hostAction(runtime,a)):[];
 const localActions=actions.filter(a=>!hostActions.includes(a));
 const disabled=formStructuralDisabled(runtime),required=localActions.filter(a=>!Object.hasOwn(disabled,a));
 // These are explicitly synthetic declaration defaults, not a provider reply.
 // Service actions are contract-only here and require separate end-to-end QA.
 const syntheticHost=runtime.ownership?{fresh:true,variables:initialApp(runtime).vars,ui:{mode:0,actions:[]}}:undefined;
 const visible=native=>{const s=mixedHostSnapshot(runtime,native,syntheticHost),values=appDashboard(runtime,s),page=appPage(runtime,s);return JSON.stringify(runtime.surface.elements
  .filter(n=>appVisible(n,page,s.vars,runtime.states.indexOf(s.state))&&projectNode(runtime.surface,n,s.vars))
  .map(n=>[n.type,(n.type==="cells"?s.cells:appNodeValue(n,s,values,runtime)),projectNode(runtime.surface,n,s.vars)]));};
 const maxPumps=(Math.ceil(runtime.program.image.fuel/64)+3)*(runtime.program.timers.length+5);
 const failures=new Set(),effective=new Set(),states=new Set(),seen=new Set(),queue=[[]];
 let cursor=0,truncated=false,timerVisible=false,executed=0;
 function replay(trace){
  const core=createComponentProgram(runtime);let state=initialApp(runtime);state.program=core.diagnostic();if(syntheticHost)state.hostReply=syntheticHost;
  function settle(){
   for(let i=0;i<maxPumps;i++){
    const values=core.pump(state.vars,state.clock);executed++;
    if(values){state.vars=values;const cells=core.gridSnapshot();if(cells!==null)state.cells=cells;}
    state.program=core.diagnostic();
    if(!state.program.live){failures.add('A reachable program event stopped without committing; existing model was preserved');return false;}
    if(state.program.phase==='idle')return true;
   }
   failures.add('Program verification exceeded its bounded pump budget');return false;
  }
  try{
   for(const item of trace){
    const before=visible(state);
    if(item==='@timer')state=appAdvance(runtime,state,Math.max(100,...runtime.program.timers.map(t=>t.delayMs),...(runtime.tick||[]).map(t=>t.every_ms)));
    else if(item==='@cooldown')state=appAdvance(runtime,state,Math.max(250,...Object.values(runtime.input?.cooldown||{})));
    else if(actionEnabled(runtime,state,item)){
     if(core.handles(item)){const captured=programInputValues(runtime,state.vars,item,syntheticHost);if(!captured||core.enqueue(item,captured)!=='ok'){failures.add('A reachable action could not be queued');return null;}state=actionAccepted(runtime,state,item);}
     else state=appAction(runtime,state,item);
    }
    if(!settle())return null;
    if(before!==visible(state)){if(item==='@timer')timerVisible=true;else if(item!=='@cooldown')effective.add(item);}
   }
   return {state,seed:core.snapshot()};
  }finally{core.cancel();}
 }
 while(cursor<queue.length&&seen.size<128&&executed<200000){
  const trace=queue[cursor++],result=replay(trace);if(!result)continue;
  const s=result.state,key=JSON.stringify([s.state,s.page,s.vars,s.history||[],result.seed.values,result.seed.outputs,localActions.map(a=>actionEnabled(runtime,s,a))]);
  if(seen.has(key))continue;seen.add(key);states.add(s.state);
  if(required.every(a=>effective.has(a))&&runtime.states.every(s=>states.has(s))&&(!runtime.program.timers.length||timerVisible))break;
  if(trace.length>=32){truncated=true;continue;}
  for(const action of localActions)if(actionEnabled(runtime,s,action))queue.push([...trace,action]);
  if(runtime.program.timers.length||(runtime.tick||[]).length)queue.push([...trace,'@timer']);
  if(Object.keys(runtime.input?.cooldown||{}).length&&localActions.some(a=>!actionEnabled(runtime,s,a)))queue.push([...trace,'@cooldown']);
 }
 truncated||=cursor<queue.length&&(seen.size>=128||executed>=200000);
 if(required.some(a=>!effective.has(a)))failures.add('Some declared inputs have no reachable visible feedback');
 if(runtime.states.some(s=>!states.has(s)))failures.add('Some declared states are unreachable');
 if(runtime.program.timers.length&&!timerVisible)failures.add('Declared program timers did not produce visible feedback');
 return {ok:!failures.size,mode:'shared-native-component-program',visibleActions:[...effective].sort(),disabledActions:disabled,reachableStates:[...states].sort(),
  explorationStates:seen.size,explorationTruncated:truncated,pumpCalls:executed,timersAdvanced:timerVisible,errors:[...failures],deviceVerified:false,hostServicesVerified:false,
  ...(runtime.ownership?{syntheticHostInputs:true,hostActionsContractOnly:hostActions,note:'Native replay uses declared defaults only; real host values, commands and device coexistence require separate acceptance.'}:{})};
}
