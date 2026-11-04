import React,{createContext,useContext,useEffect,useRef,useState} from 'react';
import {invoke} from '@tauri-apps/api/core';
import Button from '../shell/Button';
import {BACKGROUND_IDLE,createProgramBackgroundController,supportsBackgroundPrograms,supportsBackgroundProgramEngine} from './program-background.js';
import {AUTOSTART_IDLE,PROGRAM_AUTOSTART_NOTICE,createProgramAutostartController,programAutostartMessage,supportsProgramAutostart} from './program-autostart.js';
import './program-background.css';
const Context=createContext(null);
export function ProgramBackgroundProvider({usb,active=true,children,request=invoke}){
  const [state,setState]=useState(BACKGROUND_IDLE),owner=useRef(null);
  const [autostart,setAutostart]=useState(AUTOSTART_IDLE),autoOwner=useRef(null),connection=useRef(null);
  const board=String(usb?.boardDeviceId||'');
  const eligible=Boolean(active&&usb?.connected&&usb?.runtime==='esp-p4'&&supportsBackgroundPrograms(usb?.capabilities));
  const autoEligible=eligible&&supportsProgramAutostart(usb?.capabilities);
  const boot=String(usb?.capabilities?.debugBootId||'');
  useEffect(()=>{
    setState(BACKGROUND_IDLE);setAutostart(AUTOSTART_IDLE);
    if(!eligible){owner.current=null;autoOwner.current=null;connection.current=null;return undefined;}
    const controller=createProgramBackgroundController({board,invoke:request,onChange:setState});owner.current=controller;
    const auto=autoEligible?createProgramAutostartController({invoke:request,onChange:setAutostart}):null;
    autoOwner.current=auto;connection.current={owner:auto,background:controller,board,hint:boot};
    void controller.refresh();
    if(auto)void auto.refresh();
    return()=>{controller.dispose();auto?.dispose();if(owner.current===controller)owner.current=null;if(autoOwner.current===auto)autoOwner.current=null;};
  },[board,boot,eligible,autoEligible,request]);
  // Scheduling belongs to the native connection owner, regardless of this
  // page's visibility. Mount/refresh/enable NEVER issue observe or start IPC.
  useEffect(()=>{
    const controller=owner.current;
    if(!eligible||!controller||state.status!=='ready'||state.pending||state.checking||autostart.pending||!state.snapshot?.items.some(row=>!row.stopped))return undefined;
    const timer=setTimeout(()=>{void controller.refresh();},2000);
    return()=>clearTimeout(timer);
  },[eligible,state,autostart.pending]);
  const currentBackground=()=>{
    const current=owner.current,binding=connection.current;
    if(!current||binding?.background!==current||binding.board!==board||binding.hint!==boot)throw Error('background_owner_changed');
    return current;
  };
  const currentAuto=()=>{currentBackground();const current=autoOwner.current;if(!current||connection.current?.owner!==current)throw Error('autostart_owner_changed');return current;};
  const value=eligible?{state,board,capabilities:usb.capabilities,maxActive:usb.capabilities.widgetProgramBackgroundMaxActive,
    refresh:()=>Promise.allSettled([owner.current?.refresh(),autoOwner.current?.refresh()]),
    start:id=>{const current=currentBackground();if(autoOwner.current?.getState().pending)throw Error('background_state_not_ready');return current.start(id);},
    stop:async(id,observed)=>{
      const current=currentBackground();
      if(autoOwner.current?.getState().pending)throw Error('background_state_not_ready');
      // Preserve the independent observed-owner stop even if the optional PC
      // policy command is unavailable. Native global stop/preemption hooks are
      // still mandatory for stops outside this UI.
      await autoOwner.current?.suppress({boardId:board,bootId:observed.bootId});
      if(owner.current!==current)throw Error('background_owner_changed');
      return current.stop(id,observed);
    },
    autostart:autoEligible?{state:autostart,prepare:componentId=>currentAuto().prepare({boardId:board,componentId}),
      enable:target=>currentAuto().enable(target,{userConfirmed:true}),disable:()=>currentAuto().disable()}:null}:null;
  return <Context.Provider value={value}>{children}</Context.Provider>;
}
function message(state){
  if(state.status==='unknown')return '上次结果未确认，未自动重发。请先刷新设备状态。';
  if(state.status==='error')return '后台程序状态暂不可用；已有状态和存档保留，请刷新核对。';
  if(state.pending)return '正在等待设备确认；不会重复提交。';
  if(!state.snapshot)return '正在核对设备后台程序…';
  return '后台运行不依赖当前页面。主动停止后不会自动恢复；存档由设备后台处理。';
}
export function ProgramBackgroundStatus(){
  const controller=useContext(Context);const [localError,setLocalError]=useState('');
  if(!controller)return null;
  const {state}=controller,busy=state.pending||state.checking||controller.autostart?.state.pending;
  const stop=async row=>{
    setLocalError('');try{await controller.stop(row.widgetId,{bootId:state.snapshot.bootId,registration:row.registration});}
    catch{setLocalError('状态已更新，请刷新后再操作。');}
  };
  return <><section className="program-background" aria-label="设备后台程序">
    <div className="program-background__header"><strong>设备后台程序 · 上限 {controller.maxActive}</strong>
      <Button size="small" variant="ghost" disabled={busy} onClick={()=>{setLocalError('');void controller.refresh();}}>刷新状态</Button></div>
    <p role="status">{localError||message(state)}</p>
    {state.snapshot?.items.length===0&&<p>当前没有后台程序。已安装的程序组件可在详情中显式启动。</p>}
    {state.snapshot?.items.map(row=><div className="program-background__row" key={`${row.widgetId}:${row.registration}`}>
      <span><strong>{row.widgetId}</strong><small>{state.status!=='ready'?'上次状态：':''}{row.live?'运行中':row.stopped?'已停止':'等待资源退出'} · 修订 {row.revision}{row.checkpointPending?' · 存档排队中':''}</small></span>
      <span>单步峰值 {row.maxStepUs} μs</span>
      <Button size="small" variant="secondary" disabled={busy||state.status!=='ready'||row.stopped} onClick={()=>void stop(row)}>停止</Button>
    </div>)}
  </section><ProgramAutostartStatus /></>;
}
export function ProgramAutostartStatus(){
  const controller=useContext(Context);const [error,setError]=useState('');
  const auto=controller?.autostart;
  if(!auto)return null;
  const {state}=auto,busy=state.pending||controller.state.pending||controller.state.checking;
  return <section className="program-background" aria-label="PC 托管连接后启动">
    <div className="program-background__header"><strong>PC 托管 · 连接后启动</strong>
      <Button size="small" variant="ghost" disabled={busy} onClick={()=>{setError('');void controller.refresh();}}>刷新策略</Button></div>
    <p>{PROGRAM_AUTOSTART_NOTICE}</p>
    {state.policy&&<p>已授权设备 <code>{state.policy.boardId}</code> 的组件 <code>{state.policy.componentId}</code>；更新、删除组件或换设备将撤销授权。</p>}
    <p role="status">{error||programAutostartMessage(state)}</p>
    <Button size="small" variant="secondary" disabled={busy||(!state.policy&&state.status!=='unknown')} onClick={async()=>{
      setError('');try{await auto.disable();}catch{setError('停用结果未确认，请刷新策略核对。');}
    }}>停用连接后启动</Button>
    <p>停用只撤销今后的自动启动；停止当前程序请使用上方“停止”。</p>
  </section>;
}
function ProgramAutostartSettings({component,isInstalled,installing}){
  const controller=useContext(Context),auto=controller?.autostart;
  const [preview,setPreview]=useState(null),[consent,setConsent]=useState(false),[error,setError]=useState('');
  const selection=useRef(null);
  useEffect(()=>{
    const current={};selection.current=current;setPreview(null);setConsent(false);setError('');
    return()=>{if(selection.current===current)selection.current=null;};
  },[controller?.board,component?.id,component?.versionHash,isInstalled,installing]);
  if(!auto)return null;
  const state=auto.state,busy=state.pending||controller.state.pending||controller.state.checking;
  const ready=isInstalled&&!installing&&!busy&&state.status==='ready'&&controller.state.status==='ready';
  const current=preview?.boardId===controller.board&&preview?.componentId===component.id?preview:null;
  return <div className="program-autostart-settings" aria-label="连接后启动授权">
    <strong>连接后启动 · 需明确授权</strong><p>{PROGRAM_AUTOSTART_NOTICE}</p>
    {state.policy&&<p>当前授权组件：<code>{state.policy.componentId}</code>。启用其他组件会替换这条授权。</p>}
    <Button size="small" variant="secondary" disabled={!ready} onClick={async()=>{
      const selected=selection.current;setPreview(null);setConsent(false);setError('');
      try{const target=await auto.prepare(component.id);if(selection.current===selected&&selected)setPreview(target);}
      catch{if(selection.current===selected&&selected)setError('未能核对当前设备和内容，未启用。请刷新策略后再核对。');}
    }}>核对授权内容</Button>
    {current&&<div className="program-autostart-preview">
      <dl><div><dt>设备</dt><dd>{current.boardId}</dd></div><div><dt>组件</dt><dd>{current.componentId}</dd></div>
        <div><dt>内容指纹 SHA-256</dt><dd><code>{current.contentHash}</code></dd></div></dl>
      <label className="program-autostart-consent"><input type="checkbox" checked={consent} disabled={!ready} onChange={event=>setConsent(event.target.checked)}/>
        <span>我授权仅此设备和当前核对内容在连接后尝试启动。</span></label>
      <Button size="small" variant="primary" disabled={!ready||!consent} onClick={async()=>{
        if(!consent||!current)return;const selected=selection.current;setError('');
        try{const result=await auto.enable(current);if(selection.current===selected&&selected){setConsent(false);setPreview(null);
          if(result?.status!=='ready'||result.code!=='enabled')setError('启用结果未确认，请刷新策略；不会自动重发或立即启动。');}}
        catch{if(selection.current===selected&&selected)setError('启用结果未确认，请刷新策略核对。');}
      }}>明确启用连接后启动</Button>
      <p>此操作只保存授权，不会立即启动程序；每次开机的已有尝试记录会保留。</p>
    </div>}
    <p role="status">{error||programAutostartMessage(state)}</p>
  </div>;
}
export function ProgramBackgroundStart({component,isInstalled,installing}){
  const controller=useContext(Context);const [error,setError]=useState('');
  if(!controller||!supportsBackgroundProgramEngine(controller.capabilities,component?.appRuntime?.program?.engine))return null;
  const {state}=controller;
  const running=state.status==='ready'&&state.snapshot?.items.some(row=>row.widgetId===component.id&&row.live);
  const enabled=isInstalled&&!installing&&state.status==='ready'&&!state.pending&&!state.checking&&!controller.autostart?.state.pending&&!running;
  return <section className="program-background" aria-label="程序后台运行">
    <strong>运行设备已安装的版本</strong><p>这是实际设备操作，不是预览；不会重新安装、重置存档或自动录音播放。</p>
    <Button size="small" variant="secondary" disabled={!enabled} onClick={async()=>{setError('');try{await controller.start(component.id);}catch{setError('状态尚未确认，请刷新设备后台程序。');}}}>{running?'已在后台运行':state.pending?'正在确认…':'在设备后台运行'}</Button>
    {(!isInstalled||error)&&<p role="status">{error||'请先将当前组件同步到设备。'}</p>}
    {(state.status==='unknown'||state.status==='error')&&<p role="status">{message(state)}</p>}
    <ProgramAutostartSettings component={component} isInstalled={isInstalled} installing={installing}/>
  </section>;
}
