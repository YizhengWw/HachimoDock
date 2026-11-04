// Synthetic fixtures only: no Tauri IPC, user saves, device, microphone or cloud.
import React, {useEffect, useRef, useState} from 'react';
import {createRoot} from 'react-dom/client';
import {AppSurface, ComponentAppPreview} from '../../src/component-center/AppSurface.jsx';
import {initialApp, appAction} from '../../src/component-center/app-runtime.js';
import {actionEnabled} from '../../src/component-center/input-policy.js';
import formContract from '../../skills/petui/contracts/app-form-v1.json';
import navigationContract from '../../skills/petui/contracts/app-navigation-v1.json';
import viewportsContract from '../../skills/petui/contracts/app-viewports-v1.json';
import collectionContract from '../../skills/petui/contracts/collection-v1.json';
import canvasContract from '../../skills/petui/contracts/canvas-v1.json';
import mixedContract from '../../skills/petui/contracts/app-form-v2.json';
import {FormTextEditor} from '../../src/component-center/FormTextEditor.jsx';
import {formContext} from '../../src/component-center/app-form.js';
import {replaceMixedDraft} from '../../src/component-center/form-v2-core.js';
import {PROGRAM_ENGINE,encodeProgram,createProgramPreview} from '../../src/component-center/program-core.js';
import {createCollectionPreview} from '../../src/component-center/collection-core.js';
import hostCollectionContract from '../../skills/petui/contracts/host-collection-v1.json';
import {updateCollectionReply} from '../../src/component-center/host-collection.js';
import componentProgramContract from '../../skills/petui/contracts/component-program-v1.json';
import programFormContract from '../../skills/petui/contracts/component-program-form-v1.json';
import ownersContract from '../../skills/petui/contracts/variable-owners-v1.json';
import {updateOwnedHostReply} from '../../src/component-center/variable-owners.js';
import ComponentAppPanel from '../../src/component-center/ComponentAppPanel.jsx';

const texture = document.createElement('canvas'); texture.width = texture.height = 64;
const brush = texture.getContext('2d');
for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) {
  brush.fillStyle = (x + y) % 2 ? '#f2b66d' : '#64838a'; brush.fillRect(x * 8, y * 8, 8, 8);
}
const images = [{id: 'synthetic-checker', dataUrl: texture.toDataURL('image/png')}];
const text = (x, y, w, label, extra = {}) => ({type: 'text', x, y, w, h: 32, color: 8452, text: label, ...extra});
const button = (x, y, w, label, action, extra = {}) => ({type: 'button', x, y, w, h: 48, color: 14783, foreground: 65535, radius: 12, text: label, action, ...extra});
const form = structuredClone(formContract.runtime);
form.surface.background = 63421;
form.surface.elements = [
  text(24, 20, 450, 'FORM / SHARED CONTRACT'),
  {type: 'image', image: 'synthetic-checker', x: 536, y: 20, w: 64, h: 64},
  text(24, 96, 230, 'Current choice'),
  {type: 'text', x: 24, y: 132, w: 260, h: 44, color: 8452, field_bind: 'mode'},
  text(24, 192, 230, 'Current amount'),
  {type: 'text', x: 24, y: 228, w: 260, h: 44, color: 8452, field_bind: 'level'},
  text(336, 96, 230, 'Saved choice / amount'),
  {type: 'text', x: 336, y: 132, w: 80, h: 44, color: 8452, var_bind: 'choice'},
  {type: 'text', x: 456, y: 132, w: 80, h: 44, color: 8452, var_bind: 'amount'},
  text(336, 192, 230, 'Focus / editing'),
  {type: 'text', x: 336, y: 228, w: 80, h: 44, color: 8452, var_bind: 'focus'},
  {type: 'text', x: 456, y: 228, w: 80, h: 44, color: 8452, var_bind: 'editing'},
  button(24, 324, 184, 'Apply or edit', 'form.accept'),
  button(224, 324, 184, 'Discard', 'form.cancel'),
  button(424, 324, 184, 'Leave page', 'leave'),
  button(424, 408, 184, 'Back', 'back'),
  text(24, 408, 380, 'Home page: editing disabled', {page: 1}),
];
const previewComponent = {appRuntime: form, sceneSprites: images, defaultBindings: formContract.buttons};
const plainComponent = {appRuntime: form, defaultBindings: formContract.buttons};
const componentProgramRuntime=structuredClone(componentProgramContract.runtime);
componentProgramRuntime.program.actions.exceed=[200];componentProgramRuntime.transitions.push({from:'*',on:'exceed'});
componentProgramRuntime.surface.elements.push({type:'button',x:20,y:220,w:220,h:64,color:63488,text:'Exceed',action:'exceed'});
const componentProgramFixture={appRuntime:componentProgramRuntime,defaultBindings:[...componentProgramContract.buttons,{action:'exceed',label:'Exceed'}]};
const mixedProgramFixture={appRuntime:programFormContract.runtime,defaultBindings:programFormContract.buttons};
const ownersRuntime=structuredClone(ownersContract.runtime);
ownersRuntime.transitions.push({from:'*',on:'host.increment'});
ownersRuntime.surface.elements.push(text(20,240,220,'',{var_bind:'step'}),button(260,240,220,'Host','host.increment'));
const ownersFixture={appRuntime:ownersRuntime,defaultBindings:[{action:'add',label:'Add'},{action:'subtract',label:'Subtract'}]};
function OwnersPreview(){
 const [reply,setReply]=useState({fresh:true,variables:{step:5,count:999},ui:{mode:0,context:'a',actions:['host.increment']}});
 const calls=useRef([]);
 useEffect(()=>{api.drivers.owners={replace:r=>setReply(old=>updateOwnedHostReply(ownersRuntime,old,r)),
   replaceMany:replies=>{for(const r of replies)setReply(old=>updateOwnedHostReply(ownersRuntime,old,r));},received:()=>[...calls.current],clear:()=>{calls.current=[];}};},[]);
 return <article data-fixture="owners"><h2>Native instance + explicit immutable host fields</h2><div className="screen"><ComponentAppPreview component={ownersFixture} active interactive hostSnapshot={reply} onHostAction={a=>calls.current.push(a)}/></div></article>;
}
const guardedRuntime=structuredClone(ownersRuntime);
guardedRuntime.input.confirm={add:'Accept current host value?',subtract:'Accept native subtraction?'};
guardedRuntime.surface.elements.push(button(20,340,240,'Guarded subtract','subtract',{host_mask:255}));
const guardedFixture={appRuntime:guardedRuntime,defaultBindings:componentProgramContract.buttons.map(b=>({...b,label:b.action==='add'?'Add':'Subtract'}))};
const cooldownRuntime=structuredClone(componentProgramContract.runtime);
cooldownRuntime.input.cooldown={add:250};
const cooldownFixture={appRuntime:cooldownRuntime,defaultBindings:componentProgramContract.buttons};
function GuardedPreview(){
 const [reply,setReply]=useState({fresh:true,variables:{step:2},ui:{mode:0,context:'a',actions:['host.increment']}});
 useEffect(()=>{api.drivers.guards={replace:r=>setReply(old=>updateOwnedHostReply(guardedRuntime,old,r))};},[]);
 return <article data-fixture="guards"><h2>Shared physical/pointer confirmation and host publication guards</h2><div className="screen"><ComponentAppPreview component={guardedFixture} active interactive hostSnapshot={reply}/></div></article>;
}
const ownedPanelRuntime=structuredClone(programFormContract.runtime);
ownedPanelRuntime.host=true;ownedPanelRuntime.host_context='p4-host-context-v1';
ownedPanelRuntime.ownership={engine:'p4-variable-owners-v1',host:['step']};ownedPanelRuntime.program.timers=[];
ownedPanelRuntime.transitions.push({from:'*',on:'host.increment'});
const ownedPanelComponent={id:'synthetic-owned-panel',name:'Synthetic owned panel',versionHash:'fixture-1',appRuntime:ownedPanelRuntime,
 defaultBindings:programFormContract.buttons,pcApplication:{engine:'pc-app-composition-v2',service:'composition',actions:{'host.increment':'source:increment'}}};
let panelHostStep=5,panelHostEnabled=true;const panelHostRequests=[];
async function ownedPanelRequest(command,{input}){
 if(command!=='component_app_request')throw Error('Unexpected synthetic request');panelHostRequests.push(structuredClone(input));
 if(input.operation==='action'){if(input.action!=='host.increment')throw Error('Native action leaked to host');panelHostStep++;}
 if(input.operation==='disable')panelHostEnabled=false;if(input.operation==='enable')panelHostEnabled=true;
 return {enabled:panelHostEnabled,variables:{step:panelHostStep,count:999},values:{},ui:{mode:0,context:panelHostStep.toString(16),actions:['host.increment']},sources:{source:'ready'}};
}
function OwnedServicePanel(){
 useEffect(()=>{api.drivers.ownedPanel={setStep:n=>{panelHostStep=n;},requests:()=>structuredClone(panelHostRequests)};},[]);
 return <article data-fixture="ownedPanel"><ComponentAppPanel component={ownedPanelComponent} request={ownedPanelRequest}/></article>;
}
const lifecycleRuntime=structuredClone(componentProgramContract.runtime);
lifecycleRuntime.vars.pulse={type:'int',init:0};lifecycleRuntime.limits.pulse=[0,10000];
lifecycleRuntime.tick=[{every_ms:100,inc:{pulse:1}}];
lifecycleRuntime.surface.elements.push({type:'text',x:20,y:310,w:220,h:32,color:65535,var_bind:'pulse'});
const lifecycleFixture={appRuntime:lifecycleRuntime,defaultBindings:componentProgramContract.buttons};
const multi = structuredClone(navigationContract.runtime);
multi.vars = {left_offset: {type: 'int', init: 0}, right_offset: {type: 'int', init: 0}, hits: {type: 'int', init: 0}};
multi.pages = [{id: 'home'}, {id: 'detail'}];
multi.limits = {left_offset: [0, 400], right_offset: [0, 320], hits: [0, 10000]};
multi.transitions = [
  {from: '*', from_page: 'home', on: 'left.row', inc: {hits: 1}},
  {from: '*', from_page: 'home', on: 'right.row', inc: {hits: 10}},
  {from: '*', on: 'fixed', inc: {hits: 100}},
  {from: '*', on: 'left.down', inc: {left_offset: 120}},
  {from: '*', on: 'left.up', inc: {left_offset: -120}},
  {from: '*', on: 'right.down', inc: {right_offset: 200}},
  {from: '*', on: 'right.up', inc: {right_offset: -200}},
  {from: '*', from_page: 'home', on: 'open', page: {push: 'detail'}},
  {from: '*', on: 'back', page: {back: true}},
  {from: '*', from_page: 'detail', on: 'detail.only', inc: {hits: 1000}},
];
multi.surface = structuredClone(viewportsContract.surface);
multi.surface.background = 63421;
multi.surface.elements = [
  {type: 'image', image: 'synthetic-checker', view: 'left', x: 8, y: 12, w: 256, h: 128},
  {type: 'image', image: 'synthetic-checker', view: 'right', x: 8, y: 12, w: 256, h: 128},
  ...multi.surface.elements.map(n => ({...n, color: 14783, foreground: 65535, radius: 12})),
  button(160, 8, 144, 'Details', 'open'), button(480, 8, 144, 'Back', 'back'),
  button(16, 412, 288, 'Detail only', 'detail.only'),
  {type: 'text', x: 352, y: 420, w: 240, h: 32, color: 8452, var_bind: 'hits'},
];
const mixed=structuredClone(mixedContract.runtime);
const api = window.__frameworkUi = {ready: false, formContract, navigationContract, viewportsContract, runtime: {form, multi,mixed}, drivers: {}};
function Controlled({id, runtime, bindings = []}) {
  const [snapshot, setSnapshot] = useState(() => initialApp(runtime));
  const ref = useRef(snapshot); ref.current = snapshot;
  const observations = useRef([]);
  const dispatch = action => setSnapshot(s => appAction(runtime, s, action));
  useEffect(() => {
    observations.current.push(structuredClone(snapshot));
    api.drivers[id] = {state: () => structuredClone(ref.current), observations: () => structuredClone(observations.current),
      action: dispatch, actions: actions => actions.forEach(dispatch),replace:response=>setSnapshot(old=>({...old,...response})),
      reset: () => {observations.current = []; setSnapshot(initialApp(runtime));}};
  }, [snapshot]);
  return <article data-fixture={id}><h2>{id}</h2><div className="screen"><AppSurface runtime={runtime} snapshot={snapshot} images={images} bindings={bindings} active onAction={dispatch}/></div>
    <FormTextEditor key={snapshot.vars[runtime.form?.focus_var]??'none'} runtime={runtime} snapshot={snapshot} enabled
      onReplace={(field,text,context)=>setSnapshot(s=>{if(formContext(runtime,s)!==context)return s;const next=structuredClone(s);return replaceMixedDraft(runtime,next,field,text)==='applied'?next:s;})}/>
    <div className="physical" aria-label={`${id} physical controls`}>{[...new Set(runtime.transitions.map(t => t.on))].map(action => <button key={action} data-action={action} disabled={!actionEnabled(runtime, snapshot, action)} onClick={() => dispatch(action)}>{action}</button>)}</div>
    <output>{JSON.stringify({page: snapshot.page, history: snapshot.history || [], vars: snapshot.vars})}</output></article>;
}
function App() {
  const [active, setActive] = useState(true), [revision, setRevision] = useState(0);
  const [programVisible,setProgramVisible]=useState(true);
  const [componentProgramActive,setComponentProgramActive]=useState(true);
  const [mixedProgramActive,setMixedProgramActive]=useState(true),[lifecycleActive,setLifecycleActive]=useState(true);
  useEffect(() => {api.setPreviewActive = setActive; api.resetPreview = () => {setActive(true); setRevision(n => n + 1);}; api.ready = true;}, []);
  useEffect(()=>{api.setComponentProgramActive=setComponentProgramActive;},[]);
  useEffect(()=>{api.setMixedProgramActive=setMixedProgramActive;api.setLifecycleActive=setLifecycleActive;},[]);
  return <><style>{`body{margin:0;padding:24px;background:#e8edef;font:14px sans-serif;color:#24383d}main{display:flex;gap:24px;align-items:flex-start;flex-wrap:wrap}article{padding:16px;background:white;border-radius:16px;width:640px}h1{font-size:22px}h2{font-size:16px}.screen{width:640px;min-height:480px}.physical,[role=group]{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0}button{padding:8px}output{display:block;white-space:pre-wrap;font:12px monospace;max-width:640px;overflow-wrap:anywhere}`}</style>
    <h1>Offline synthetic framework acceptance — no services or user data</h1><main>
      <section><button data-program-visibility onClick={()=>setProgramVisible(v=>!v)}>Toggle synthetic program</button>{programVisible&&<ProgramProbe/>}</section>
      <article data-fixture="componentProgram"><h2>Installed declaration / production lazy component preview</h2>
        <button data-component-program-visibility onClick={()=>setComponentProgramActive(v=>!v)}>Toggle component lifecycle</button>
        <div className="screen"><ComponentAppPreview component={componentProgramFixture} active={componentProgramActive} interactive/></div></article>
      <article data-fixture="mixedProgram"><h2>Mixed program, text and navigation</h2><div className="screen"><ComponentAppPreview component={mixedProgramFixture} active={mixedProgramActive} interactive/></div></article>
      <article data-fixture="lifecycleProgram"><h2>Foreground lifecycle / ordinary ticks</h2><div className="screen"><ComponentAppPreview component={lifecycleFixture} active={lifecycleActive} interactive/></div></article>
      <OwnersPreview/>
      <GuardedPreview/>
      <article data-fixture="cooldown"><h2>Actual C program cooldown</h2><div className="screen"><ComponentAppPreview component={cooldownFixture} active interactive/></div></article>
      <OwnedServicePanel/>
      <article data-fixture="preview"><h2>ComponentAppPreview / shared form contract</h2><div className="screen"><ComponentAppPreview key={revision} component={previewComponent} active={active} interactive/></div></article>
      <Controlled id="form" runtime={form} bindings={formContract.buttons}/>
      <Controlled id="multi" runtime={multi}/>
      <Controlled id="canvas" runtime={drawing}/>
      <Controlled id="mixed" runtime={mixed} bindings={mixedContract.buttons}/>
      <HostCollection/>
      <HostListControl/>
      <NativeCollection/>
      <article data-fixture="plain"><h2>ComponentAppPreview / no sprite resources</h2><div className="screen"><ComponentAppPreview component={plainComponent} active interactive/></div></article>
    </main></>;
}
/* Authoring acceptance only: this is NOT a production component route or
 * package program loader. Every click performs at most one runtime phase. */
function ProgramProbe(){
  const ref=useRef(null);if(!ref.current)ref.current=createProgramPreview(encodeProgram({engine:PROGRAM_ENGINE,schema:1,cells:1,fuel:64,code:[['CONST',0,0,0,0],['LOAD',1,0,0,0],['EVENT',2,0,0,0],['ADD',1,1,2,0],['STORE',0,1,0,0],['OUTPUT',0,1,0,0],['HALT',0,0,0,0]]}),{quantum:1});
  const p=ref.current,sequence=useRef(0),[display,setDisplay]=useState(()=>({phase:'idle',snapshot:p.snapshot()}));
  const current=useRef(display);current.current=display;
  useEffect(()=>{const driver={state:()=>structuredClone(current.current),pump:()=>p.pump()};api.drivers.program=driver;
    return()=>{p.cancel();if(api.drivers.program===driver)delete api.drivers.program;};},[p]);
  const publish=phase=>setDisplay({phase,snapshot:p.snapshot()});
  return <article data-fixture="program"><h2>Bounded program / explicit offline authoring only</h2>
    <button data-program-enqueue onClick={()=>{const result=p.enqueue(sequence.current+1,[5]);if(result==='ok')sequence.current++;publish(result);}}>Queue +5</button>
    <button data-program-pump onClick={()=>{const result=p.pump();publish(result==='complete'?p.takeReport().result:result);}}>One execution slice</button>
    <button data-program-cancel onClick={()=>{p.cancel();publish('cancelled');}}>Cancel</button>
    <output>{JSON.stringify(display)}</output>
  </article>;
}
const collectionRuntime=structuredClone(collectionContract.runtime);
collectionRuntime.surface.background=63421;
collectionRuntime.surface.elements=[
  text(24,20,560,'STABLE-KEY COLLECTION / HOST SNAPSHOTS'),
  button(24,100,560,'','host.row_a',{var_bind:'rowa'}),
  button(24,170,560,'','host.row_b',{var_bind:'rowb'}),
  text(24,250,560,'Selected identity remains stable'),
  text(24,292,560,'',{var_bind:'chosen'}),
];
// Authored bound nodes must not also carry a literal text value, even empty.
for(const node of collectionRuntime.surface.elements)if(node.var_bind)delete node.text;
/* Same fixed C collection as the device building block, shown through the real
 * existing bounded AppSurface. Synthetic authoring fixture ONLY: no new device
 * package/collection capability, persistent records or IPC are enabled. */
const nativeRows=[{key:'a',label:'Alpha'},{key:'b',label:'Beta'},{key:'c',label:'Gamma'}];
function NativeCollection(){
  const model=useRef(null),epoch=useRef(0);if(!model.current)model.current=createCollectionPreview(nativeRows,{pageSize:2});
  const capture=()=>{
    const v=model.current.status(),actions=[];
    if(v.rows[0].present)actions.push('host.row_a');if(v.rows[1].present)actions.push('host.row_b');
    if(v.canNext)actions.push('host.next');if(v.canPrevious)actions.push('host.previous');
    if(v.canUp)actions.push('host.up');if(v.canDown)actions.push('host.down');if(v.selectionKey)actions.push('host.clear');
    return {...initialApp(collectionRuntime),vars:{rowa:v.rows[0].label,rowb:v.rows[1].label,chosen:v.selectedLabel,count:v.count},
      ui:{mode:0,context:`native-collection-${epoch.current}`,actions},collection:v,saved:model.current.snapshot()};
  };
  const [snapshot,setSnapshot]=useState(capture),current=useRef(snapshot);current.current=snapshot;
  const dispatch=action=>{
    const op={'host.row_a':'row_0','host.row_b':'row_1','host.next':'page_next','host.previous':'page_previous',
      'host.up':'up','host.down':'down','host.clear':'clear_selection'}[action];
    if(!op||!current.current.ui.actions.includes(action)||!model.current.action(op))return;
    epoch.current++;setSnapshot(capture());
  };
  useEffect(()=>{
    api.drivers.nativeCollection={state:()=>structuredClone(current.current),action:dispatch,
      replace:rows=>{const next=model.current.withRows(rows);model.current=next;epoch.current++;setSnapshot(capture());}};
  },[snapshot]);
  return <article data-fixture="nativeCollection"><h2>Shared native collection / offline authoring only</h2>
    <div className="screen"><AppSurface runtime={collectionRuntime} snapshot={snapshot} active onAction={dispatch}/></div>
    <div className="physical">{Object.keys(collectionContract.declaration.actions).map(action=><button key={action} data-action={action}
      disabled={!snapshot.ui.actions.includes(action)} onClick={()=>dispatch(action)}>{action}</button>)}</div>
    <output>{JSON.stringify({saved:snapshot.saved,vars:snapshot.vars})}</output></article>;
}
function HostCollection(){
  const [snapshot,setSnapshot]=useState(()=>({...initialApp(collectionRuntime),vars:collectionContract.initial,
    ui:{mode:0,context:'initial-records',actions:['host.row_a','host.row_b','host.next','host.up','host.down']}}));
  const current=useRef(snapshot);current.current=snapshot;const received=useRef([]);
  useEffect(()=>{api.drivers.collection={state:()=>structuredClone(current.current),
    received:()=>[...received.current],clear:()=>{received.current=[];},
    replace:response=>setSnapshot(old=>({...old,...response}))};},[]);
  return <article data-fixture="collection"><h2>Collection / actual AppSurface with synthetic host replies</h2>
    <div className="screen"><AppSurface runtime={collectionRuntime} snapshot={snapshot} active onAction={a=>received.current.push(a)}/></div>
  </article>;
}
const hostListRuntime=structuredClone(hostCollectionContract.runtimeBase);
hostListRuntime.surface.collection_view=structuredClone(hostCollectionContract.config);
function HostListControl(){
  const reply=model=>({collectionView:model,variables:{count:model?.count||0},ui:{mode:0,context:'abc',actions:['host.pick_a','host.pick_b']}});
  const [data,setData]=useState(()=>updateCollectionReply({},reply(hostCollectionContract.snapshot)));
  const current=useRef(data);current.current=data;const received=useRef([]);
  useEffect(()=>{api.drivers.hostList={state:()=>structuredClone(current.current),received:()=>[...received.current],clear:()=>{received.current=[];},
    replace:model=>setData(old=>updateCollectionReply(old,reply(model))),
    replaceMany:models=>{for(const model of models)setData(old=>updateCollectionReply(old,reply(model)));}};},[]);
  const snapshot={...initialApp(hostListRuntime),vars:data.variables,ui:data.ui,collectionView:data.collectionView,pointerEpoch:data._collectionEpoch};
  return <article data-fixture="hostList"><h2>Host collection control / actual production virtual rows</h2>
    <div className="screen"><AppSurface runtime={hostListRuntime} snapshot={snapshot} active onAction={a=>received.current.push(a)}/></div></article>;
}
const drawing=structuredClone(navigationContract.runtime);
drawing.vars=structuredClone(canvasContract.runtime.vars);drawing.pages=[{id:'home'},{id:'detail'}];
drawing.dashboard={title:'Bounded Drawing'};
drawing.transitions=[{from:'*',on:'move',set:{x:40,y:28,w:24,h:10}},
  {from:'*',from_page:'home',on:'open',page:{push:'detail'}},{from:'*',on:'back',page:{back:true}}];
drawing.surface={engine:'p4-app-surface-v1',graphics:'p4-app-graphics-v1',bindings:'p4-variable-bindings-v1',background:63421,
  canvas:{...structuredClone(canvasContract.valid[0]),x:40,y:100,page:0},
  elements:[{type:'panel',x:0,y:0,w:640,h:480,color:63421},text(24,20,560,'SHARED C CANVAS / BOUNDED DRAWING'),
    button(24,350,184,'Move drawing','move'),button(224,350,184,'Details','open'),button(424,350,184,'Back','back')]};
createRoot(document.getElementById('root')).render(<App/>);
