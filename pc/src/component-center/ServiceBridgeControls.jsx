import React,{useState} from 'react';
import {open} from '@tauri-apps/plugin-dialog';
import './service-bridge-controls.css';
import {bridgeEventArguments} from './service-bridge-controls-model.js';

const labels={play:'播放选中歌曲',toggle:'播放 / 暂停',pause:'暂停',resume:'继续播放',stop:'停止',previous:'上一首',next:'下一首',volume_up:'音量 +',volume_down:'音量 −',cycle_mode:'播放模式',volume:'音量',mode:'播放模式',seek:'跳转位置',seek_percent:'播放进度',remove:'移除选中歌曲',up:'向上选择',down:'向下选择'};
const argumentLabels={percent:'播放进度 (%)',positionMs:'位置 (毫秒)',volume:'音量',key:'稳定记录键',mode:'播放模式'};
export default function ServiceBridgeControls({component,data,enabled,busy,onCommand,onError}){
 const [drafts,setDrafts]=useState({});
 const commands=data?.commands||{},available=data?.ui?.actions||[];
 const localMedia=Object.values(data?.serviceDescriptors||{}).some(v=>v.permissions?.localAudioImport);
 async function importFiles(alias){
  try{
   const paths=await open({multiple:true,directory:false,filters:[{name:'本地音频',extensions:['mp3','m4a','wav','flac','ogg','aac']}]});
   if(paths?.length)await onCommand('import',{service:alias,paths:Array.isArray(paths)?paths:[paths]});
  }catch(error){onError?.(String(error));}
 }
 function submit(event,action,command){
  event.preventDefault();let values;
  try{values=bridgeEventArguments(command,drafts[action]);}catch(error){onError?.(String(error.message));return;}
  const prompt=component.appRuntime?.input?.confirm?.[action];
  if(prompt&&!window.confirm(prompt))return;
  onCommand('action',{action,data:values,...(prompt?{confirm:true,context:data?.ui?.context}:{})});
 }
 return <section className="service-bridge-controls" aria-label="通用服务控制">
  <header><div><span className="service-bridge-controls__eyebrow">HOST SERVICES</span><h3>{localMedia?'曲库与播放控制':'服务控制'}</h3></div><span className="service-bridge-controls__badge">显式操作 · 不自动播放</span></header>
  {Object.entries(data?.serviceDescriptors||{}).filter(([,v])=>v.permissions?.localAudioImport).map(([alias])=><div className="service-bridge-controls__import" key={alias}>
   <div><strong>本地曲库</strong><p>在这台电脑选择音频，可同时读取同名歌词与封面。原文件不会随组件分享。</p></div>
   <button type="button" disabled={!enabled||busy} onClick={()=>importFiles(alias)}>＋ 导入音频</button>
  </div>)}
  <div className="service-bridge-controls__grid">{Object.entries(commands).filter(([,c])=>!/^row_\d+$/.test(c.method)).map(([action,c])=><form key={action} onSubmit={e=>submit(e,action,c)}>
   {Object.values(c.arguments||{}).filter(a=>a.value?.event).map(a=>{const key=a.value.event;return <label key={key}>{argumentLabels[key]||key}
    <input aria-label={argumentLabels[key]||key} type={a.type==='int'?'number':a.type==='bool'?'checkbox':'text'} min={a.min} max={a.max} maxLength={63} required={a.type!=='bool'}
     value={a.type==='bool'?undefined:drafts[action]?.[key]??''} checked={a.type==='bool'?!!drafts[action]?.[key]:undefined}
     onChange={e=>setDrafts(old=>({...old,[action]:{...old[action],[key]:a.type==='bool'?e.target.checked:e.target.value}}))}/></label>;})}
   <button className={c.method==='remove'?'service-bridge-controls__destructive':''} disabled={!enabled||busy||!available.includes(action)}>{labels[c.method]||component.defaultBindings?.find(b=>b.action===action)?.label||action}</button>
  </form>)}</div>
  <p className="service-bridge-controls__footnote">列表操作使用当前选中记录。设备退出、断线或语音打断后，需要再次明确播放；操作结果未知时不自动重发。</p>
 </section>;
}
