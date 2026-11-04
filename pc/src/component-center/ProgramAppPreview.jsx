import React,{useEffect,useRef,useState} from 'react';
import {AppSurface} from './AppSurface.jsx';
import {initialApp,appAction,appAdvance} from './app-runtime.js';
import {actionEnabled,actionAccepted} from './input-policy.js';
import {formContext,formCancel} from './app-form.js';
import {MIXED_FORM_ENGINE,replaceMixedDraft} from './form-v2-core.js';
import {FormTextEditor} from './FormTextEditor.jsx';
import {createComponentProgram} from './component-program.js';
import {hostAction,mixedHostSnapshot,programInputValues} from './variable-owners.js';
/* No Tauri/services/credentials/device IO. The same C VM runs one slice per
 * explicit browser turn; immutable checkpoints remain private to this preview.
 * Effects/timers cannot run in inactive cards. A stopped instance never resumes. */
export default function ProgramAppPreview({component,active,interactive,hostSnapshot,onHostAction}){
  const runtime=component.appRuntime,initial=()=>({...initialApp(runtime),program:{live:false,phase:'preparing'}});
  const [state,setState]=useState(initial),[error,setError]=useState(''),[suspended,setSuspended]=useState(!active);
  const current=useRef(state),program=useRef(null),seed=useRef(null),paused=useRef(!active);
  const inputAction=useRef(null),[confirming,setConfirming]=useState(false);
  const publish=s=>{current.current=s;setState(s);};
  useEffect(()=>{seed.current=null;publish(initial());setError('');},[runtime,component.versionHash]);
  useEffect(()=>{
    paused.current=!active;setSuspended(!active);
    if(!active){const next=formCancel(runtime,structuredClone(current.current));next.program={...next.program,live:false,...(seed.current?{phase:'stopped'}:{})};publish(next);return;}
    let core;
    try{core=createComponentProgram(runtime,{seed:seed.current,nowMs:current.current.clock});program.current=core;publish({...current.current,program:core.diagnostic()});}
    catch{setError('受限程序或存档不兼容；原预览数据保留');return;}
    const stop=()=>{if(paused.current)return;paused.current=true;setSuspended(true);seed.current=core.snapshot();core.cancel();if(program.current===core)publish({...formCancel(runtime,structuredClone(current.current)),program:core.diagnostic(),pointerEpoch:(current.current.pointerEpoch||0)+1});};
    const timer=setInterval(()=>{
      if(program.current!==core||paused.current)return;
      const next=appAdvance(runtime,current.current,20),values=core.pump(next.vars,next.clock);
      if(values){next.vars=values;const cells=core.gridSnapshot();if(cells!==null)next.cells=cells;next.pointerEpoch=(next.pointerEpoch||0)+1;}
      next.program=core.diagnostic();publish(next);
    },20);
    const visibility=()=>{if(document.hidden)stop();};
    window.addEventListener('blur',stop);document.addEventListener('visibilitychange',visibility);
    return()=>{clearInterval(timer);window.removeEventListener('blur',stop);document.removeEventListener('visibilitychange',visibility);
      seed.current=core.snapshot();core.cancel();if(program.current===core)program.current=null;};
  },[runtime,component.versionHash,active]);
  const action=(a,confirmation)=>{
    const reply=hostSnapshot,view=mixedHostSnapshot(runtime,current.current,reply);
    if(!active||paused.current||!interactive||!actionEnabled(runtime,view,a))return;
    if(hostAction(runtime,a)){onHostAction?.(a,confirmation);return;}
    const core=program.current;
    if(core?.handles(a)){const inputs=programInputValues(runtime,current.current.vars,a,reply);if(!inputs)return;const accepted=core.enqueue(a,inputs)==='ok';publish({...actionAccepted(runtime,current.current,a,accepted),program:core.diagnostic()});}
    else publish(appAction(runtime,current.current,a));
  };
  const view=mixedHostSnapshot(runtime,state,hostSnapshot);
  if(error)return <div role="alert">{error}</div>;
  return <div aria-label="受限程序组件预览" data-component-program-phase={state.program?.phase}>
    <AppSurface componentId={component.id} runtime={runtime} snapshot={view} images={component.sceneSprites} bindings={component.defaultBindings} active={active&&!suspended} inputActionRef={inputAction} onConfirmationChange={setConfirming} onAction={interactive?action:undefined}/>
    {runtime.form?.engine===MIXED_FORM_ENGINE&&interactive&&<FormTextEditor runtime={runtime} snapshot={state} enabled={active&&!suspended}
      onReplace={(field,text,context)=>{if(paused.current||formContext(runtime,current.current)!==context)return;const next=structuredClone(current.current);if(replaceMixedDraft(runtime,next,field,text)==='applied')publish(next);}}/>}
    {interactive&&<div role="group" aria-label="程序组件实体按键预览">{(component.defaultBindings||[]).map(b=><button key={b.action} disabled={!active||suspended||(confirming?!['button.sw1.short_press','button.sw2.short_press'].includes(b.event):!actionEnabled(runtime,view,b.action))} onClick={()=>inputAction.current?.(b.action)}>{b.label||b.action}</button>)}</div>}
    {state.program?.live===false&&state.program?.phase==='stopped'&&<p role="status">程序已停止，已提交的内容保留；重新打开组件可建立新实例。</p>}
    {state.program?.live&&state.program?.canEnqueue===false&&<p role="status">程序队列已满，请等待已接受的操作完成；不会丢弃或重复执行操作。</p>}
  </div>;
}
