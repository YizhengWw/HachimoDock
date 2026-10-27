import React, {useEffect, useRef, useState} from "react";
import {invoke} from "@tauri-apps/api/core";
import {Play, Plus, Square, Trash2} from "lucide-react";
import {MediaSurface} from "./MediaSurface.jsx";
import {EMPTY_MEDIA, mediaReadiness} from "./media-surface-model.js";
import "./component-media.css";

export default function ComponentMediaPanel({component, request=invoke}) {
  const componentId=component.id, config=component.pcExtension.panel;
  const permissions=component.pcExtension.permissions, sections=new Set(config.sections);
  const [enabled,setEnabled]=useState(false),[checking,setChecking]=useState(true);
  const [s,setS]=useState(EMPTY_MEDIA),[query,setQuery]=useState("");
  const [results,setResults]=useState([]),[searched,setSearched]=useState(false);
  const [busy,setBusy]=useState(false),[acting,setActing]=useState(false),[error,setError]=useState("");
  const [pollError,setPollError]=useState('');
  const epoch=useRef(0),lifetime=useRef(0),actingRef=useRef(false);
  useEffect(()=>{
    let stopped=false,timer;lifetime.current++;epoch.current++;actingRef.current=false;
    setChecking(true);setEnabled(false);setS(EMPTY_MEDIA);setResults([]);setSearched(false);setError("");setBusy(false);setActing(false);
    async function poll(){
      const version=epoch.current;
      try {
        if(!actingRef.current){
          const value=await request("component_extension_request",{componentId,input:{operation:"status"}});
          if(!stopped&&version===epoch.current){setEnabled(value.enabled!==false);setS(value);setChecking(false);setPollError('');}
        }
      } catch(e){if(!stopped&&version===epoch.current){setPollError(String(e));setChecking(false);}}
      if(!stopped)timer=setTimeout(poll,1000);
    }
    poll();return()=>{stopped=true;lifetime.current++;clearTimeout(timer);};
  },[componentId,component.versionHash,request]);
  async function command(input){
    if(actingRef.current)return;
    actingRef.current=true;epoch.current++;const active=lifetime.current;
    setActing(true);setError("");
    try {
      const value=await request("component_extension_request",{componentId,input});
      if(active!==lifetime.current)return;
      if(input.operation==="disable"){setEnabled(false);setS(EMPTY_MEDIA);setResults([]);setSearched(false);}
      else {setS(value);setEnabled(value.enabled!==false);}
    } catch(e){if(active===lifetime.current)setError(String(e));} finally{if(active===lifetime.current){actingRef.current=false;setActing(false);}}
  }
  async function search(e){
    e.preventDefault();if(busy||!enabled||!query.trim())return;
    const active=lifetime.current,version=epoch.current;
    setBusy(true);setError("");setResults([]);setSearched(false);
    try {
      const reply=await request("component_extension_request",{componentId,input:{operation:"search",query:query.trim()}});
      if(active===lifetime.current&&version===epoch.current){setResults(reply.stations||[]);setSearched(true);}
    } catch(e){if(active===lifetime.current&&version===epoch.current)setError(String(e));} finally{if(active===lifetime.current)setBusy(false);}
  }
  async function openSource(e){
    if(!window.__TAURI_INTERNALS__)return;
    e.preventDefault();try{await request("open_external_url",{url:config.attributionUrl});}
    catch{setError("浏览器未能打开，请稍后重试");}
  }
  const saved=new Set(s.stations.map(t=>t.id)),ready=mediaReadiness(s,enabled);
  const canPlay=enabled&&!pollError&&s.connected&&s.supported!==false&&!acting;
  const playing=["playing","buffering"].includes(s.state);
  return <section className="component-media" aria-label="组件媒体管理">
    <h3>{config.title}</h3>
    <p>感谢 <a href={config.attributionUrl} onClick={openSource} target="_blank" rel="noreferrer">{config.attribution}</a> 提供电台目录服务。</p>
    <p>{config.description}</p>
    <div className="component-media__setup" role="status"><strong>{checking?"正在检查播放条件…":ready.title}</strong><p>{ready.message}</p></div>
    <details className="component-media__permission">
      <summary>服务与数据使用说明</summary>
      <p>组件服务默认启用，更新后无需再次授权。{component.pcExtension.discovery?.autoPlayOnEnter?"自动备好本地与全国电台，进入设备组件即可收听；本 PC 面板不会自动播放。":"打开组件后自动加载电台列表，在设备选择、切台即可；不会自动播放。"}</p>
      <p>搜索访问：{permissions.networkHosts.join("、")}。播放访问搜索结果的公网音频地址；收藏和历史仅保存在本机。若系统 DNS 失败或返回代理虚拟地址，使用阿里公共 DNS／Cloudflare 安全 DNS 解析公网电台域名（不发送搜索词、对话或凭据）。不会获取任意文件或命令执行权限。</p>
    </details>
    {!checking&&!enabled&&!error&&<div className="component-media__permission">
      <p>此组件服务已手动停用，收藏和历史仍保留。</p>
      <button className="component-media__primary" disabled={acting} onClick={()=>command({operation:"enable"})}>{acting?"正在开启…":"重新开启服务"}</button>
    </div>}
    <div className="component-media__top">
      <MediaSurface images={component.sceneSprites} surface={component.mediaSurface} snapshot={{...s,message:enabled?s.message:ready.message}} command={canPlay?command:undefined}/>
      <aside>
        {sections.has("volume")&&<label>音量<select disabled={!enabled||acting} value={s.volume} onChange={e=>command({operation:"volume",volume:Number(e.target.value)})}>{Array.from({length:21},(_,i)=><option key={i} value={i*5}>{i*5}%</option>)}</select></label>}
        {sections.has("sleep")&&<label>睡眠定时<select disabled={!enabled||acting} value={s.sleepMinutes} onChange={e=>command({operation:"sleep",minutes:Number(e.target.value)})}><option value="0">不设置</option>{[15,30,60,90].map(n=><option key={n} value={n}>{n} 分钟后停止</option>)}</select></label>}
        <button disabled={!enabled||!playing||acting} onClick={()=>command({operation:"stop"})}><Square size={14}/> 停止收听</button>
      </aside>
    </div>
    {enabled&&s.message&&<p role="status">{s.message}</p>}
    {enabled&&component.pcExtension.discovery&&<p role="status">{s.location||"正在识别城市"} · 已备好 {s.queue?.length||0} 个电台 · 直接在设备切台{ s.discovering?" · 更新中（已有列表仍可用）":""}</p>}
    {(error||pollError)&&<div role="alert" className="component-media__error">{error||pollError}{pollError&&' · 正在自动重新核对播放状态'}</div>}
    {sections.has("search")&&<form onSubmit={search}><input aria-label="城市或条目名称" disabled={!enabled} value={query} maxLength={30} onChange={e=>setQuery(e.target.value)} placeholder={config.searchPlaceholder}/><button disabled={!enabled||busy||!query.trim()}>{busy?"搜索中…":"搜索电台"}</button></form>}
    {sections.has("search")&&searched&&<><h4>搜索结果</h4>{!results.length&&<p>没有找到可用电台，可换成城市全名、广播名称或“新闻”等关键词。</p>}{results.map(t=><div className="component-media__row" key={t.id}>
      <span>{t.name}<small>{t.city} · {t.hls?"HLS":t.codec||"直播"}</small></span>
      <button disabled={!canPlay} onClick={()=>command({operation:"play",id:t.id})}><Play size={16}/>播放</button>
      <button disabled={acting||saved.has(t.id)} onClick={()=>command({operation:"add",id:t.id})}><Plus size={16}/>{saved.has(t.id)?"已收藏":"收藏"}</button>
    </div>)}</>}
    {sections.has("library")&&<><h4>我的收藏 · {s.stations.length}/20</h4>{!s.stations.length&&<p>暂无收藏。搜索后可以直接播放，也可以收藏后用设备按键切台。</p>}{s.stations.map(t=><div className="component-media__row" key={t.id}><span>{t.name}<small>{t.city}</small></span><button disabled={!canPlay} aria-label={`播放${t.name}`} onClick={()=>command({operation:"play",id:t.id})}><Play size={16}/></button><button disabled={!enabled||acting} aria-label={`移除${t.name}`} onClick={()=>command({operation:"remove",id:t.id})}><Trash2 size={16}/></button></div>)}</>}
    {sections.has("history")&&s.history.length>0&&<><h4>最近收听</h4><div className="component-media__history">{s.history.map(t=><button disabled={!canPlay} key={t.id} onClick={()=>command({operation:"play",id:t.id})}>{t.name}</button>)}</div></>}
    {enabled&&<button className="component-media__disable" disabled={acting} onClick={()=>command({operation:"disable"})}>停用 PC 扩展（保留收藏）</button>}
  </section>;
}
