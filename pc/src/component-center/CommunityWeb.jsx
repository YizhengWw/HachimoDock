import React,{useEffect,useRef,useState} from 'react';
import {invoke} from '@tauri-apps/api/core';
import {COMMUNITY_ORIGIN,parseCommunityMessage} from './community-bridge.js';
import {commentStorage,readCommentIdentity,saveCommentName} from './comment-identity.js';
import {FAVORITES_KEY,readFavorites,updateFavorites,versionState} from './community-model.js';
import {useExitProtection} from '../lib/exit-protection.js';
import WorkflowConfirm from './WorkflowConfirm.jsx';

const request=(action,fields={})=>invoke('community_request',{input:{action,...fields}});
// Development uses a same-origin reverse proxy, never an arbitrary URL parameter.
const origin=import.meta.env.DEV?location.origin:COMMUNITY_ORIGIN;
export default function CommunityWeb({view='discover',localItems,inventory,onImported,onOpenLocal}){
  const frame=useRef(null),current=useRef(null),confirmation=useRef(null),[confirm,setConfirm]=useState(null);
  const [ready,setReady]=useState(false),[failed,setFailed]=useState(false),[revision,setRevision]=useState(0),[dirty,setDirty]=useState(false),[busy,setBusy]=useState(false);
  current.current={localItems,inventory,onImported,onOpenLocal};
  useExitProtection(busy?'社区下载或提交尚未完成':dirty?'社区评论或昵称尚未保存':'');
  useEffect(()=>{
    let alive=true,initialized=false,activeWrites=0;const channel=crypto.randomUUID(),inflight=new Set(),done=new Set();
    setReady(false);setFailed(false);setDirty(false);
    const timer=setTimeout(()=>{if(alive&&!initialized)setFailed(true);},15000);
    const post=data=>frame.current?.contentWindow?.postMessage({...data,v:1,channel},origin);
    function local(work){const item=current.current.localItems.find(x=>x.id===work.id&&!x.isDeviceOnly),state=versionState(work,item,current.current.inventory);return {item,state};}
    async function status(id){const work=await request('detail',{id}),{item,state}=local(work);return {hasLocal:!!item,localLabel:state.localLabel,deviceLabel:state.deviceLabel,blocked:!!(state.localNewer||state.contentConflict||state.versionUnknown||(item&&!state.updateAvailable)),downloadLabel:state.contentConflict?'同版本内容冲突':state.versionUnknown?'版本待核对':state.localNewer?'本机版本更高':item?(state.updateAvailable?'下载新版本':'已加入本机'):'下载到我的组件'};}
    async function execute(action,fields){
      if(action==='profile'){saveCommentName(commentStorage(),fields.name);return readCommentIdentity(commentStorage());}
      if(action==='favorite'){const work=await request('detail',{id:fields.id});const values=updateFavorites(readFavorites(localStorage),work);localStorage.setItem(FAVORITES_KEY,JSON.stringify(values));return values.map(x=>({id:x.id,name:x.name}));}
      if(action==='status')return status(fields.id);
      if(action==='configure'){const item=current.current.localItems.find(x=>x.id===fields.id&&!x.isDeviceOnly);if(!item)throw Error('请先下载组件');current.current.onOpenLocal(item);return {ok:true};}
      if(action==='download'){
        const work=await request('detail',fields),state=await status(fields.id);if(state.blocked)throw Error(state.downloadLabel);
        const accepted=await new Promise(resolve=>{confirmation.current=resolve;setConfirm(work);});confirmation.current=null;setConfirm(null);
        if(!accepted||!alive)throw Error('已取消下载，未修改本机或设备');
        const result=await request('download',{...fields,expectedContentHash:work.contentHash});await current.current.onImported(result.entry);return {ok:true};
      }
      if(action==='comment')return request('comment',{...fields,name:readCommentIdentity(commentStorage()).displayName});
      return request(action,fields);
    }
    async function receive(event){
      if(event.origin!==origin||event.source!==frame.current?.contentWindow)return;
      const d=event.data;if(!d||d.v!==1)return;
      if(d.type==='pet-community-ready'){
        if(initialized)return;initialized=true;clearTimeout(timer);setReady(true);setFailed(false);
        post({type:'pet-community-init',view,context:{identity:readCommentIdentity(commentStorage()),favorites:readFavorites(localStorage).map(x=>({id:x.id,name:x.name}))}});return;
      }
      if(d.type==='pet-community-dirty'&&d.channel===channel&&typeof d.value==='boolean'){setDirty(d.value);return;}
      const parsed=parseCommunityMessage(event,frame.current?.contentWindow,origin,channel);if(!parsed)return;
      const {action,fields,requestId}=parsed;if(inflight.has(requestId)||done.has(requestId))return;
      const write=['download','comment','like','profile','favorite','configure'].includes(action);
      if(inflight.size>=16||(write&&activeWrites)){post({type:'pet-community-response',requestId,error:'上一个操作尚未完成，请稍后再试'});return;}
      inflight.add(requestId);if(write){activeWrites++;setBusy(true);}
      try{const result=await execute(action,fields);if(alive)post({type:'pet-community-response',requestId,result});}
      catch(error){if(alive)post({type:'pet-community-response',requestId,error:typeof error==='string'?error:error.message||'操作未完成，请重试'});}
      finally{inflight.delete(requestId);done.add(requestId);if(done.size>1000)done.delete(done.values().next().value);if(write){activeWrites--;if(alive)setBusy(false);}}
    }
    window.addEventListener('message',receive);return()=>{alive=false;clearTimeout(timer);window.removeEventListener('message',receive);confirmation.current?.(false);confirmation.current=null;};
  },[view,revision]);
  return <section className="community-web" aria-label={view==='favorites'?'我的收藏':'社区广场'}>
    {!ready&&<div className="community-web-loading" role="status">{failed?'社区页面暂时未能加载；本机组件不受影响。':'正在加载社区网页…'}{failed&&<button onClick={()=>setRevision(x=>x+1)}>重新连接</button>}</div>}
    <iframe key={`${view}:${revision}`} ref={frame} src={`${origin}/community/#${view}`} title="哈基米社区网页" sandbox="allow-scripts allow-same-origin allow-forms" referrerPolicy="no-referrer" onError={()=>setFailed(true)}/>
    {confirm&&<WorkflowConfirm title="下载社区组件" description={`将从社区下载「${confirm.name}」${confirm.version}，校验后加入本机组件库。不会自动安装到设备，也不会开放本机文件或设备控制权限。`} confirmLabel="确认下载" cancelLabel="取消下载" onCancel={()=>confirmation.current?.(false)} onConfirm={()=>confirmation.current?.(true)}/>}
  </section>;
}
