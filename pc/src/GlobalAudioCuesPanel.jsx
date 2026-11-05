/** Global audio controls, independent of appearance selection and video state. */
import React,{useEffect,useId,useState} from 'react';
import {invoke} from '@tauri-apps/api/core';
import {Upload,UploadCloud,Trash2,Volume2} from 'lucide-react';
import {compressWavFileForBoard} from './lib/audio-cue.js';
import {defaultAudioCueSrcForFamily} from './lib/builtin-appearances.js';
import {GLOBAL_AUDIO_STATES,globalAudioDirty,globalAudioRevision,markGlobalAudioDirty,migrateBuiltinGlobalAudioCues,readGlobalAudioCue,saveGlobalAudioCue,removeGlobalAudioCue} from './lib/global-audio-cues.js';
import {useExitProtection} from './lib/exit-protection.js';
const labels={idle:'休息',working:'工作',done:'任务完成',waiting_user:'等待操作',error:'出错',welcome:'欢迎',permission_request:'请求审批',permission_approved:'审批通过',permission_denied:'审批拒绝',permission_expired:'审批超时',session_terminated:'会话停止'};
export default function GlobalAudioCuesPanel({boardDeviceId,currentFamily}){
 const [family,setFamily]=useState(()=>GLOBAL_AUDIO_STATES.includes(currentFamily)?currentFamily:'done'),[busy,setBusy]=useState(false),[error,setError]=useState(''),[message,setMessage]=useState(''),[url,setUrl]=useState(''),[custom,setCustom]=useState(false),[revision,setRevision]=useState(0),[dirty,setDirty]=useState(globalAudioDirty);
 const id=useId();useExitProtection(busy?'提示音操作尚未完成':'');
 useEffect(()=>{if(GLOBAL_AUDIO_STATES.includes(currentFamily))setFamily(currentFamily);},[currentFamily]);
 useEffect(()=>{setError('');setMessage('');},[family]);
 useEffect(()=>{let disposed=false,objectUrl='';
  setUrl('');setCustom(false);
  (async()=>{try{if(!window.__TAURI_INTERNALS__){setUrl(defaultAudioCueSrcForFamily(family));return;}
    await migrateBuiltinGlobalAudioCues();const bytes=await readGlobalAudioCue(family);
    if(disposed)return;setCustom(Boolean(bytes));setDirty(globalAudioDirty());
    objectUrl=bytes?URL.createObjectURL(new Blob([bytes],{type:'audio/wav'})):'';
    setUrl(objectUrl||defaultAudioCueSrcForFamily(family));
   }catch(e){if(!disposed)setError(String(e?.message||e));}})();
  return()=>{disposed=true;if(objectUrl)URL.revokeObjectURL(objectUrl);};
 },[family,revision]);
 async function change(action){if(busy)return;setBusy(true);setError('');setMessage('');
  try{await action();setRevision(n=>n+1);setDirty(globalAudioDirty());setMessage('已保存，点击“下发音效”应用到所有形象。');}
  catch(e){setError(String(e?.message||e));}finally{setBusy(false);}}
 async function upload(event){const file=event.target.files?.[0];event.target.value='';if(!file)return;
  await change(async()=>{if(!/\.wav$/i.test(file.name))throw new Error('请上传 WAV 格式文件');await saveGlobalAudioCue(family,await compressWavFileForBoard(file));});}
 async function sync(){if(busy)return;setBusy(true);setError('');setMessage('');
  try{const revision=globalAudioRevision();const result=await invoke('usb_sync_appearance',{appearanceId:'global-audio-cues',boardDeviceId,audioOnly:true});
   if(!result?.ok)throw new Error(result?.error||'设备未确认音效下发');markGlobalAudioDirty(false,revision);setDirty(globalAudioDirty());setMessage('全局提示音已下发，适用于所有形象。');}
  catch(e){setError(String(e?.message||e));}finally{setBusy(false);}}
 return <section className="detail-side-section detail-side-section--audio"><div className="detail-audio-config">
  <div className="detail-audio-config__header"><span><Volume2 size={15}/>全局提示音</span><span className="muted small">所有形象通用</span></div>
  <select aria-label="提示音状态" value={family} disabled={busy} onChange={e=>setFamily(e.target.value)}>{GLOBAL_AUDIO_STATES.map(f=><option key={f} value={f}>{labels[f]||f}</option>)}</select>
  {url?<audio className="detail-audio-config__player" src={url} controls/>:<div className="muted small">此状态未设置提示音</div>}
  <div className="detail-audio-config__actions"><input id={id} type="file" accept=".wav,audio/wav" className="detail-audio-config__input" disabled={busy} onChange={upload}/>
   <label className={`btn-ghost${busy?' is-disabled':''}`} htmlFor={busy?undefined:id}><Upload size={14}/>上传 WAV</label>
   {custom&&<button type="button" className="btn-ghost danger" disabled={busy} onClick={()=>change(()=>removeGlobalAudioCue(family))}><Trash2 size={14}/>恢复默认</button>}
   <button type="button" className="btn-primary" disabled={busy||!boardDeviceId} onClick={sync}><UploadCloud size={14}/>{busy?'处理中…':'下发音效'}</button>
  </div>{(error||message||dirty)&&<div role={error?'alert':'status'} className={`message-banner ${error?'message-banner--error':''}`}>{error||message||'提示音尚未下发'}</div>}
 </div></section>;
}
