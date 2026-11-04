import React,{useEffect,useRef,useState,useMemo} from "react";
import {invoke} from "@tauri-apps/api/core";
import {AppSurface,ComponentAppPreview} from "./AppSurface.jsx";
import {initialApp} from "./app-runtime.js";
import {updateCollectionReply} from './host-collection.js';
import {updateOwnedHostReply} from './variable-owners.js';
import {bridgeReply,validImageRef} from "./service-bridge.js";
import ServiceBridgeControls from './ServiceBridgeControls.jsx';
import "./component-app.css";

const EMPTY_CITY={city:"",lat:0,lon:0};
const LABELS={voice:"语音补充",details:"详情 / 隐藏",previous:"上一项",next:"下一项",random:"换一条 / 确认分类",category:"分类 / 返回",depart:"开始旅行",collection:"明信片",refresh:"更新预报",todo:"近期待办",record:"开始 / 结束录音",save:"确认保存",delete:"删除笔记",speak:"朗读 / 停止",favorite:"收藏 / 取消",toggle:"播放 / 停止",timer:"切换定时",up:"音量 +",down:"音量 −"};
export function permissionText(p={},notice=""){
  const result=["组件服务默认启用，更新后无需再次授权。记录只保存在这台电脑，不随资源包或社区发布；系统权限仍由操作系统管理。"];
  if(p.networkHosts?.length)result.push(`联网访问 ${p.networkHosts.join("、")}。${notice}`);
  if(p.newsGeneration)result.push("使用 Pet Manager 已配置的对话模型及联网检索，可能产生调用费用；仅发送公开主题和新闻摘录，不发送聊天、个人资料或桌面任务。进入设备组件自动整理，PC 预览不自动请求；结果附原文链接与发布时间。");
  if(p.microphone)result.push("仅在按下录制后使用设备麦克风，最多 28 秒；音频发给你配置的云端 ASR，转写须手动确认保存。不会写入桌面 Agent 输入框。");
  if(p.speechProvider&&!p.microphone)result.push(p.autoRead?"在设备进入组件或换条后，自动将当前知识正文分段发送给已配置的语音合成服务并朗读；打开 PC 预览不会播放，不发送其他笔记或聊天内容。":"按下朗读后，将该知识卡片文字发送给你配置的语音合成服务；不发送其他笔记或聊天内容。");
  if(p.deviceAudio)result.push("通过 USB 使用设备扬声器。退出组件、断线或进入语音后停止，不会自动恢复。");
  if(p.todos)result.push(p.todoWrites?"读取本机待办；仅在你确认具体事项后标记完成或撤销完成，不新增、不删除事项。":p.microphone?"可在你确认后创建统一待办；删除笔记不删除关联待办。":"可读取本机近期待办，内容可能显示在设备上。");
  if(p.desktopReceipts)result.push("只读取当前设备跟随 Agent 的任务回执；默认隐藏正文。语音补充仍遵守客户端原有权限确认。");
  if(p.desktopInput)result.push("可向这台电脑当前前台应用发送资源包声明的白名单组合键（无文本输入、无宏）。默认关闭：需要在下方开启后才真正按键；macOS 需在系统设置授予辅助功能权限；实时对话期间不发送。");
  return result;
}
export default function ComponentAppPanel({component,active=false,request=invoke}){
  const [data,setData]=useState(null),[error,setError]=useState(""),[pollError,setPollError]=useState(""),[busy,setBusy]=useState(false),[city,setCity]=useState(EMPTY_CITY),[editing,setEditing]=useState(null);
  const [fetchHosts,setFetchHosts]=useState('');
  const [serviceImages,setServiceImages]=useState({});
  const mounted=useRef(false),acting=useRef(false),epoch=useRef(0),cityDirty=useRef(false);
  const [hostExpired,setHostExpired]=useState(true);
  useEffect(()=>{setHostExpired(!data);if(!data)return;const timer=setTimeout(()=>setHostExpired(true),10000);return()=>clearTimeout(timer);},[data]);
  const id=component.id,service=component.pcApplication.service;
  const viewId=useMemo(()=>crypto.randomUUID(),[id,component.versionHash]);
  const updateReply=(old,v,merge=false)=>component.pcApplication.engine==="pc-service-bridge-v1"?bridgeReply(merge?{...old,...v}:v):component.appRuntime.ownership?updateOwnedHostReply(component.appRuntime,old,v,merge):component.appRuntime?.surface?.collection_view?updateCollectionReply(old,v,merge):merge?{...old,...v}:v;
  useEffect(()=>{mounted.current=true;epoch.current++;acting.current=false;cityDirty.current=false;let timer,stopped=false;setData(null);setError("");setPollError("");setEditing(null);setBusy(false);setCity(EMPTY_CITY);
    async function poll(){const version=epoch.current;try{if(!acting.current){const v=await request("component_app_request",{componentId:id,input:{operation:"status",includeNotes:true,viewId,versionHash:component.versionHash}});if(!stopped&&version===epoch.current){setData(old=>updateReply(old,v));setPollError("");if(!cityDirty.current&&v.state?.city?.city)setCity(v.state.city);}}}catch(e){if(!stopped&&version===epoch.current)setPollError(String(e));}if(!stopped)timer=setTimeout(poll,2000);}
    poll();return()=>{mounted.current=false;stopped=true;epoch.current++;clearTimeout(timer);if(service==="bridge")request("component_app_request",{componentId:id,input:{operation:"detach",viewId}}).catch(()=>{});};
  },[id,component.versionHash,request]);
  async function command(operation,extra={}){if(acting.current)return;acting.current=true;const version=++epoch.current;setBusy(true);setError("");
    const mutation=operation==="action"&&["pc-app-composition-v2","pc-app-composition-v3","pc-service-bridge-v1"].includes(component.pcApplication.engine)?{eventId:crypto.randomUUID(),context:data?.ui?.context}:{};
    try{const v=await request("component_app_request",{componentId:id,input:{operation,includeNotes:true,viewId,versionHash:component.versionHash,...mutation,...extra}});if(mounted.current&&version===epoch.current){setData(old=>updateReply(old,v,true));if(operation==="edit")setEditing(null);if(["configure","locate"].includes(operation)){cityDirty.current=false;if(v.state?.city?.city)setCity(v.state.city);}}}
    catch(e){if(mounted.current&&version===epoch.current)setError(String(e));}finally{if(version===epoch.current){acting.current=false;if(mounted.current)setBusy(false);}}}
  const imageIdentity=JSON.stringify(data?.presentation?.images||{});
  useEffect(()=>{let stopped=false;setServiceImages({});
    for(const [alias,reference] of Object.entries(data?.presentation?.images||{})){
      if(!validImageRef(reference))continue;
      request("component_app_request",{componentId:id,input:{operation:"asset",reference}}).then(image=>{if(!stopped&&mounted.current&&image.key===reference.key&&image.revision===reference.revision)setServiceImages(old=>({...old,[alias]:image}));}).catch(()=>{});
    }return()=>{stopped=true;};
  },[id,component.versionHash,imageIdentity,request]);
  async function link(e,url){if(!window.__TAURI_INTERNALS__)return;e.preventDefault();try{await request("open_external_url",{url});}catch{setError("浏览器未打开，请稍后重试");}}
  const base=useMemo(()=>initialApp(component.appRuntime),[component.appRuntime,component.versionHash]);
  const enabled=data?.enabled===true&&!pollError,snapshot={...base,vars:{...base.vars,...data?.variables},values:data?.values||{},data:data?.data,presentation:data?.presentation,serviceImages,_bridgeReceivedAt:data?._bridgeReceivedAt,collectionView:data?.collectionView,pointerEpoch:data?._collectionEpoch||0,ui:busy?{mode:6,actions:[]}:!enabled?{mode:7,actions:[]}:data?.ui},content=data?.content||[],selected=(data?.state?.selected||0)%Math.max(1,content.length),current=data?.knowledge?.current||content[selected];
  const profile=component.pcApplication.location||data?.locationProfile,cities=profile?.cities||[],t=key=>profile?.text?.[key]||"";
  const mixed=!!component.appRuntime.ownership||service==="fetch",hostSnapshot={fresh:enabled&&!busy&&!hostExpired,variables:data?.variables,values:data?.values,ui:busy?{mode:6,actions:[]}:data?.ui,collectionView:data?.collectionView,data:data?.data,_hostEpoch:data?._hostEpoch};
  const cityMode=data?.state?.auto_city&&data?.state?.geo?.error&&data.state.geo.error!==t("locatingNote")?data.state.geo.error:data?.state?.geo?.source==="manual"?"跟随共享位置 · 手动城市":data?.state?.city_from_ip?t("autoMode"):data?.state?.auto_city?t("autoPending"):t("manualMode");
  return <section className="component-app" aria-label={`${component.name}应用服务`}>
    <div className="component-app__heading"><div><h3>{component.name}</h3><p>标准 PetUI 资源包 · 设备界面 + 受控 PC 服务</p></div><span>{pollError?"状态暂不可用":!data?"检查中":enabled?"已启用":"已手动停用"}</span></div>
    <details className="component-app__permissions"><summary>服务与数据使用说明</summary>{permissionText(data?.permissions,t("dataNotice")).map(t=><p key={t}>{t}</p>)}</details>
    {data?.enabled===false&&<div className="component-app__permissions"><p>此组件服务已手动停用，本机记录仍保留。</p><button disabled={busy} onClick={()=>command("enable")}>重新开启服务</button></div>}
    <div className="component-app__layout">{mixed?<ComponentAppPreview component={component} active={active} interactive={true} hostSnapshot={hostSnapshot} onHostAction={enabled&&!busy?(a,confirmation)=>command("action",{action:a,...confirmation}):undefined}/>:<AppSurface active={active} componentId={component.id} runtime={component.appRuntime} snapshot={snapshot} images={component.sceneSprites} bindings={component.defaultBindings} onAction={enabled&&!busy&&service!=="journey"?(a,confirmation)=>command("action",{action:a,...confirmation}):undefined}/>}<div className="component-app__controls">
      <p>{service==="journey"?"目的地直接在设备上选择：点击上一站/下一站或拨动摇杆，确认后出发。旅途中可以预选下一站，不会重置当前旅程；此处只展示状态。":"设备上操作与下方按钮使用同一条服务链路。播放和录音前，请在设备打开此组件。"}</p>
      {service==="todos"&&<p>直接点击预览或设备上的事项查看全文；在详情中确认完成，也可以撤销完成。摇杆上下选事项、左右翻页，切换筛选可查看已完成事项。完整待办仍在本机统一保存。</p>}
      {service==="hotkeys"&&data?.hotkeys&&<div className="component-app__permissions" aria-label="快捷键发送开关">
        <label><input type="checkbox" checked={data.hotkeys.sendEnabled===true} disabled={busy||!enabled} onChange={e=>command("configure",{send:e.target.checked})}/> 允许向这台电脑发送快捷键（当前 {data.hotkeys.platform==="macos"?"macOS":data.hotkeys.platform==="windows"?"Windows":"不支持的平台"}）</label>
        {data.hotkeys.platform==="macos"&&data.hotkeys.accessibility===false&&<p role="alert">macOS 尚未授予辅助功能权限：系统设置 › 隐私与安全性 › 辅助功能 中允许 Pet Manager 后才会真正按键。</p>}
        {data.hotkeys.platform==="unsupported"&&<p role="alert">此平台不支持发送快捷键；组件只记录按下。</p>}
        <p>只会发送资源包声明的组合键；设备显示“已发送”仅表示系统已收到按键事件，不保证目标软件执行。</p>
      </div>}
      {service==="bridge"&&<ServiceBridgeControls key={`${id}:${component.versionHash}`} component={component} data={data} enabled={enabled&&!hostExpired} busy={busy} onCommand={command} onError={setError}/>}
      {service==="composition"&&data?.sources&&<div aria-label="组合数据源诊断"><p role="status">{Object.entries(data.sources).map(([name,phase])=>`${name}：${({ready:"可用",preparing:"更新中",error:"暂不可用，已有记录保留",busy:"队列繁忙"})[phase]||"不可用"}`).join("；")}</p>{data.bindingErrors?.length>0&&<p role="alert">部分字段暂不可用，已显示资源包声明的备用值：{data.bindingErrors.join("、")}</p>}</div>}
      {!["journey","todos","bridge"].includes(service)&&<div className="component-app__actions">{Object.entries(component.pcApplication.actions).filter(([,op])=>op!=="delete"&&!(service!=="news"&&component.pcApplication.autoRead&&["previous","next"].includes(op))).map(([action,op])=><button key={action} disabled={!enabled||busy||(Array.isArray(data?.ui?.actions)&&!data.ui.actions.includes(action))} onClick={()=>{const prompt=component.appRuntime.input?.confirm?.[action];if(prompt){const context=data?.ui?.context;if(window.confirm(prompt))command("action",{action,confirm:true,context});}else command("action",{action});}}>{component.defaultBindings?.find(b=>b.action===action)?.label||(service==="notes"&&op==="todo"?"转为待办":LABELS[op]||op)}</button>)}</div>}
      {service==="fetch"&&<form aria-label="网络来源白名单" onSubmit={e=>{e.preventDefault();command("configure",{hosts:fetchHosts.split(/[\s,，]+/).filter(Boolean)});}}>
        <p>显式启用服务并允许精确 HTTPS 主机后，PC 在组件隐藏时继续周期更新；停用服务或退出 PC 客户端后停止。空白名单不联网。</p>
        <label>允许的主机<input aria-label="允许的主机" value={fetchHosts} placeholder="example.org" onChange={e=>setFetchHosts(e.target.value)} disabled={busy}/></label><button disabled={busy}>保存网络白名单</button>
      </form>}
      {service==="weather"&&profile&&<form onSubmit={e=>{e.preventDefault();command("configure",{city});}}>
        <p role="status">{data?.state?.city?.city||t("unspecified")} · {cityMode}。{t("syncHint")}</p>
        <button type="button" disabled={!enabled||busy} onClick={()=>command("locate")}>{t("autoButton")}</button>
        <label>{t("selectLabel")}<select aria-label={t("selectLabel")} value={!data?.state?.city?.city&&!cityDirty.current?-1:cities.findIndex(c=>c.city===city.city&&c.lat===city.lat&&c.lon===city.lon)} disabled={!enabled||busy} onChange={e=>{const chosen=cities[Number(e.target.value)];if(chosen){cityDirty.current=true;setCity(chosen);command("configure",{city:chosen});}}}><option value={-1} disabled>{data?.state?.city?.city||t("selectPlaceholder")}</option>{cities.map((c,i)=><option key={c.city} value={i}>{c.city}</option>)}</select></label>
        <details><summary>{t("customLabel")}</summary>
        <label>{t("cityLabel")}<input value={city.city} maxLength={20} disabled={!enabled||busy} onChange={e=>{cityDirty.current=true;setCity({...city,city:e.target.value});}}/></label>
        <label>{t("latLabel")}<input type="number" required step="0.001" min="-90" max="90" value={city.lat} disabled={!enabled||busy} onChange={e=>{cityDirty.current=true;setCity({...city,lat:e.target.value===""?"":Number(e.target.value)});}}/></label>
        <label>{t("lonLabel")}<input type="number" required step="0.001" min="-180" max="180" value={city.lon} disabled={!enabled||busy} onChange={e=>{cityDirty.current=true;setCity({...city,lon:e.target.value===""?"":Number(e.target.value)});}}/></label>
        <button disabled={!enabled||busy||!city.city.trim()||city.lat===""||city.lon===""}>{t("saveButton")}</button>
        </details>
        <p>{t("geoNotice")}</p><p>{t("forecastNotice")}</p>
        {profile.links.map(item=><p key={item.url}><a href={item.url} onClick={e=>link(e,item.url)} target="_blank" rel="noreferrer">{item.label}</a></p>)}
      </form>}
      {enabled&&<button disabled={busy} onClick={()=>command("disable")}>停用服务（保留本机记录）</button>}
    </div></div>
    {busy&&<p role="status">正在处理，不会重复派发…</p>}{(error||pollError)&&<p role="alert" className="component-app__error">{error||pollError}</p>}
    {enabled&&service==="news"&&<div className="component-app__reading">
      <h4>过去 24 小时 · 九条新鲜事</h4><p>AI 科技、商业财经、娱乐各三条。播报采用当前宠物音色和全局音量；左右换条，上下翻页，退出停止。</p>
      {data.news?.generating&&<p role="status">正在联网核对来源并整理，不会重复提交。</p>}
      {data.news?.error&&<p role="alert">{data.news.error}</p>}
      {data.news?.playback&&<p role={data.news.playback.phase==="error"?"alert":"status"}>播报状态：{({idle:"尚未开始",waiting:"等待播放",preparing:"准备语音",buffering:"缓冲中",playing:"正在播放",stopped:"已主动停止",interrupted:"已中断",completed:"本期已播完",error:"播放失败"})[data.news.playback.phase]||"等待播放"}{data.news.playback.message?` · ${data.news.playback.message}`:""}</p>}
      {(data.news?.items||[]).map((item,index)=><article key={item.url}><h4>{index+1}. {item.title}</h4><p>{item.text}</p><small>发布时间：{new Date(item.publishedAt*1000).toLocaleString()} · </small><a href={item.url} onClick={e=>link(e,item.url)} target="_blank" rel="noreferrer">{item.source||"查看原文"}</a></article>)}
    </div>}
    {enabled&&["knowledge","books"].includes(service)&&current&&<div className="component-app__reading">
      {data.playback&&<p role={data.playback.phase==="error"?"alert":"status"}>朗读状态：{({idle:"尚未开始",waiting:"等待播放",preparing:"准备语音",buffering:"缓冲中",playing:"正在播放",stopped:"已停止",completed:"已读完",error:"播放失败"})[data.playback.phase]||"尚未开始"}{data.playback.message?` · ${data.playback.message}`:""}</p>}
      {data.knowledge&&<p>{data.knowledge.categories.map(c=>`${c.name} ${c.count}条`).join(" · ")}<br/>共 {data.knowledge.total} 条。设备左右翻页，切换分类后随机抽取；本轮尽量不重复。</p>}
      <h4>{current.category?`${current.category} · `:""}{current.title}</h4><p className="component-app__article-text">{current.text}</p>
      <a href={current.url} onClick={e=>link(e,current.url)} target="_blank" rel="noreferrer">查看原始来源 · {current.source}</a>
      {current.license&&<p>{service==="books"?"原创阅读导引与评论，不是原书全文或作者授权摘要；原书版权归原权利人，导读不替代阅读原书。":"百科节选，已整理段落与标点空白；"}{service!=="books"&&current.license}，内容版本 {current.revision}。全文可在本页阅读；{component.pcApplication.autoRead?"设备进入或换条后分段自动朗读全文，退出即停止；摇杆左右翻页。":"设备“读本页”只朗读当前两段。"}</p>}
      {current.license&&<p>内容用于一般知识学习，不代替医疗、心理诊断或投资建议；有争议和时效性的信息请核对原文来源。</p>}
    </div>}
    {enabled&&service==="journey"&&<div className="component-app__collection"><h4>目的地明信片 · 已带回 {data.state?.favorites?.length||0}/{content.length}</h4><p>四张默认明信片可以先欣赏；完成旅行后标记为已带回，不计入虚构的旅行次数。</p>{content.map(c=><div key={c.id}><b>{c.title}</b><small>{data.state?.favorites?.includes(c.id)?" · 旅行带回":" · 默认预览"}</small><p>{c.text}</p></div>)}</div>}
    {enabled&&service==="notes"&&<div className="component-app__notes"><h4>本机笔记 · {data.notes?.length||0}</h4><p>转写草稿不自动保存。编辑或删除笔记不会修改关联待办。</p>{(data.notes||[]).map(n=><div key={n.id}>{editing?.id===n.id?<form onSubmit={e=>{e.preventDefault();command("edit",{noteId:n.id,text:editing.text});}}><textarea aria-label="编辑笔记" value={editing.text} maxLength={1800} disabled={busy} onChange={e=>setEditing({...editing,text:e.target.value})}/><button disabled={busy||!editing.text.trim()}>保存修改</button><button type="button" disabled={busy} onClick={()=>setEditing(null)}>取消编辑</button></form>:<><p>{n.text}</p><button disabled={busy} onClick={()=>setEditing({id:n.id,text:n.text})}>编辑</button></>}<small>{new Date(n.created).toLocaleString()} {n.todo?"· 已转待办":""}</small><button disabled={busy} onClick={()=>{if(window.confirm("删除这条本机笔记？关联待办会保留，此操作无法撤销。"))command("delete",{noteId:n.id,confirm:true});}}>删除</button></div>)}</div>}
  </section>;
}
