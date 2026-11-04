import {dSurface,dSnapshot,dAction,visibleRows} from './d-surface.js';
import {TEXT_FORM_ENGINE,replaceTextDraft} from './form-v3-core.js';
import {listen} from '@tauri-apps/api/event';
import React,{useEffect,useLayoutEffect,useId,useState,useMemo,useRef,useCallback} from "react";
import {rgb565} from "./media-surface-model.js";
import {initialApp,appAction,appAdvance,appDashboard,appPage,appVisible,appTile,appNodeValue,appNodeMeter} from "./app-runtime.js";
import icons from "./app-icons.json";
import {glyphs,keyHint} from './app-typography.js';
import {ClassicGameLayer} from './ClassicGameLayer.jsx';
import {createClassicCore} from './classic-core.js';
import classicUrl from './classic-core.wasm?url';
import DataSurfaceLayer from './DataSurfaceLayer.jsx';
import {invoke} from '@tauri-apps/api/core';
import SceneSurfaceLayer from './SceneSurfaceLayer.jsx';
import {createSceneCore} from './scene-core.js';
import {createSideCore} from './side-core.js';
import sideCoreUrl from './side-core.wasm?url';
import SideSceneLayer from './SideSceneLayer.jsx';
import CanvasLayer from './CanvasLayer.jsx';
import ParticleLayer from './ParticleLayer.jsx';
import {createParticleActionQueue,previewOwnedValue,clearPreviewOwner} from './preview-lifecycle.js';
import {keyframeImage} from './keyframe-background.js';
import {actionEnabled,actionAccepted,swipeAction,pressAction} from './input-policy.js';
import {viewportContext,surfaceViewports,nodeViewport,projectNode,hitNode} from './app-viewport.js';
import {formCancel,formContext,formOperation} from './app-form.js';
import {replaceMixedDraft,MIXED_FORM_ENGINE} from './form-v2-core.js';
import {FormTextEditor} from './FormTextEditor.jsx';
import {hostCollectionSurface,hostCollectionContext} from './host-collection.js';
import {bridgeSnapshot,serviceImage,sliderData,SERVICE_BRIDGE} from "./service-bridge.js";
import {hostAction,hostDependent,hostGeometryReady,mixedHostSnapshot} from './variable-owners.js';
import CellsLayer from './CellsLayer.jsx';
import SurfaceMediaLayer from './SurfaceMediaLayer.jsx';

const fontFamily='"PingFang SC","Microsoft YaHei",sans-serif';
const emptyImages=Object.freeze([]);
const ProgramAppPreview=React.lazy(()=>import('./ProgramAppPreview.jsx'));
let measureContext;
function measure(text,size,weight=400,atlas){
  if(atlas?.size)return [...text].reduce((sum,c)=>sum+(atlas.has(c)?atlas.get(c).metrics[0]*size/32:measure(c,size,weight)),0);
  measureContext ||= typeof document!=="undefined"?document.createElement("canvas").getContext("2d"):null;
  if(measureContext){measureContext.font=`${weight} ${size}px ${fontFamily}`;return measureContext.measureText(text).width;}
  return [...text].reduce((s,c)=>s+(c.codePointAt(0)>255?size:size*.55),0);
}
function wrap(text,width,size,max,weight,atlas){
  const chars=[...String(text??"")],rows=[];let line="";
  for(let i=0;i<chars.length;i++){
    if(measure(line+chars[i],size,weight,atlas)>width){
      if(rows.length===max-1){while(line&&measure(line+"...",size,weight,atlas)>width)line=[...line].slice(0,-1).join("");return [...rows,line+"..."];}
      rows.push(line);line="";
    }line+=chars[i];
  }return [...rows,line].slice(0,max);
}
function Icon({name,x,y,size,color}){return <g transform={`translate(${x} ${y}) scale(${size/24})`} fill="none" stroke={color} strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">{icons[name]?.map((points,i)=><polyline key={i} points={points.map(p=>p.join(",")).join(" ")}/>)}</g>;}
function AtlasLine({text,atlas,x,y,size,color,weight,id}){
  const scale=size/32,characters=[...text],bounds=characters.map(c=>atlas.get(c)?.metrics).filter(m=>m&&m[1]!==m[2]);
  const top=bounds.length?Math.min(...bounds.map(m=>m[1])):0,bottom=bounds.length?Math.max(...bounds.map(m=>m[2])):32;
  const gy=y-(bottom-top)*scale/2-top*scale;let pen=x;
  return characters.map((c,i)=>{const g=atlas.get(c),at=pen;pen+=measure(c,size,weight,atlas);
    if(!g)return <text key={i} x={at} y={y} dominantBaseline="central" fill={color} fontSize={size} fontWeight={weight} fontFamily={fontFamily}>{c}</text>;
    const key=`${id}-${i}`;
    const sourceScale=size/g.cellHeight,width=g.cellWidth*sourceScale;
    return <g key={i}><defs><clipPath id={`${key}-clip`}><rect x={at} y={gy} width={width} height={size}/></clipPath><mask id={key} maskUnits="userSpaceOnUse" x={at} y={gy} width={width} height={size} style={{maskType:'alpha'}}><image href={g.dataUrl} x={at-g.x*sourceScale} y={gy-g.y*sourceScale} width={g.frame_width*sourceScale} height={g.frame_height*sourceScale}/></mask></defs><rect x={at} y={gy} width={width} height={size} fill={color} mask={`url(#${key})`} clipPath={`url(#${key}-clip)`}/></g>;
  });
}
function GraphicLabel({node:n,value,atlas}){
  const id=useId().replaceAll(':','');
  const size=n.font_size||16,lh=n.line_height||Math.ceil(size*1.25),button=n.type==="button",pad=n.padding??(button?16:0),weight=n.weight||400;
  const isz=n.icon_size||20,gap=n.gap??9,iw=button&&n.icon?isz+(String(value||"").length?gap:0):0;
  const width=n.w-2*pad-iw;
  if(width<1)return null; // draw_app_label also skips labels with no usable width.
  const rows=n._flowLine?[String(value??'')]:wrap(value,width,size,button?1:Math.max(1,Math.min(8,Math.floor(n.h/lh))),weight,atlas);
  const va=n.valign||(button?"center":"top"),align=n.align||(button?"center":"left"),block=rows.length*lh;
  const y=n.y+(va==="center"?(n.h-block)/2:va==="bottom"?n.h-block:0),color=rgb565(button?(n.foreground??n.color):n.color);
  return rows.map((row,i)=>{const textWidth=measure(row,size,weight,atlas),total=textWidth+(i===0?iw:0),x=n.x+pad+(align==="center"?(n.w-2*pad-total)/2:align==="right"?n.w-2*pad-total:0),tx=x+(i===0?iw:0),ty=y+i*lh+lh/2;
    return <g key={i} data-label={row}>{i===0&&iw>0&&<Icon name={n.icon} x={x} y={n.y+(n.h-isz)/2} size={isz} color={color}/>}<rect data-text-box x={tx} y={ty-size/2} width={textWidth} height={size} fill="transparent"/><title>{row}</title>{atlas?.size?<AtlasLine text={row} atlas={atlas} x={tx} y={ty} size={size} color={color} weight={weight} id={`${id}-${i}`}/>:<text x={tx} y={ty} dominantBaseline="central" fill={color} fontSize={size} fontWeight={weight} fontFamily={fontFamily}>{row}</text>}</g>;
  });
}

// Firmware uses a 16px face and a 32px line box. Native font metrics vary slightly.
function lines(text,width,scale,height){
  const rows=[],max=Math.max(1,Math.floor(height/(32*scale)));let line="",used=0;
  for(const ch of String(text??"")){const w=(ch.codePointAt(0)>255?17:9)*scale;
    if(ch==="\n"||used+w>width){rows.push(line);line="";used=0;if(ch==="\n")continue;}
    line+=ch;used+=w;
  }if(line)rows.push(line);
  return rows.slice(0,max).map((r,i)=>i===max-1&&rows.length>max?r.slice(0,-1)+"…":r);
}
export function AppSurface({componentId,runtime,snapshot,images=emptyImages,bindings=[],active=false,onAction:dispatchAction,onDrag,onHold,onConfirmationChange,inputActionRef}){
  const prefix=useId().replaceAll(":","");
  const [,bridgeFrame]=useState(0);
  useEffect(()=>{if(!active||runtime.surface?.services!==SERVICE_BRIDGE)return;const timer=setInterval(()=>bridgeFrame(n=>(n+1)%1000000),100);return()=>clearInterval(timer);},[active,runtime.surface?.services]);
  snapshot=bridgeSnapshot(runtime,snapshot,performance.now());
  snapshot=dSnapshot(runtime,snapshot);
  const surface=dSurface(runtime,snapshot,hostCollectionSurface(runtime.surface,snapshot.collectionView));
  const collectionContext=runtime.surface?.collection_view?hostCollectionContext(snapshot.collectionView):'';
  const scroll=viewportContext(surface,snapshot.vars),svg=useRef(null),wheelAt=useRef(-Infinity);
  const press=useRef(null);
  const previewOwner=useMemo(()=>({}),[componentId,runtime,images]);
  const particleActions=useMemo(()=>createParticleActionQueue(),[previewOwner]);
  const [documentVisible,setDocumentVisible]=useState(()=>typeof document==='undefined'||!document.hidden);
  const particleActive=active&&documentVisible&&(typeof document==='undefined'||!document.hidden);
  const particlePage=appPage(runtime,snapshot);
  particleActions.configure(surface.particles,particleActive,particlePage);
  const particleGeneration=particleActions.generation();
  useEffect(()=>{
    const visibility=()=>{particleActions.configure(surface.particles,active&&!document.hidden,particlePage);setDocumentVisible(!document.hidden);};
    document.addEventListener('visibilitychange',visibility);
    return()=>{document.removeEventListener('visibilitychange',visibility);particleActions.clear();};
  },[particleActions,surface.particles,active,particlePage]);
  const [pressedNode,setPressedNode]=useState(-1);
  const [confirmation,setConfirmation]=useState(null),[toast,setToast]=useState(null);
  const visibleToast=previewOwnedValue(toast,previewOwner);
  useEffect(()=>{if(!componentId||!window.__TAURI_INTERNALS__)return;let stopped=false,off,timer;
    listen('component-notify',event=>{
      if(stopped||event.payload.componentId!==componentId)return;
      setToast({owner:previewOwner,value:event.payload});clearTimeout(timer);
      timer=setTimeout(()=>setToast(current=>clearPreviewOwner(current,previewOwner)),5000);
    }).then(f=>{if(stopped)f();else off=f;}).catch(()=>{});
    return()=>{stopped=true;off?.();clearTimeout(timer);};
  },[componentId,previewOwner]);
  useEffect(()=>{onConfirmationChange?.(!!confirmation);return()=>onConfirmationChange?.(false);},[!!confirmation,onConfirmationChange]);
  const atlas=useMemo(()=>glyphs(surface,images),[surface,images]);
  const values=runtime.ownership?{...snapshot.values,...appDashboard(runtime,snapshot)}:{...appDashboard(runtime,snapshot),...snapshot.values},page=appPage(runtime,snapshot);
  const stateIndex=runtime.states.indexOf(snapshot.state);
  const editingContext=formContext(runtime,snapshot);
  useEffect(()=>{const p=press.current;
    if(p?.hold&&p.allowStart&&p.page===page&&snapshot.state===runtime.scene?.active_state){p.state=snapshot.state;p.allowStart=false;return;}
    cancel();setConfirmation(null);
  },[runtime,images,page,snapshot.state,runtime.ownership?null:snapshot.ui?.context,runtime.ownership?null:snapshot.ui?.mode,scroll,editingContext,snapshot.pointerEpoch,runtime.ownership?null:collectionContext]);
  useEffect(()=>{
    if(!runtime.ownership)return;
    const p=press.current,n=surface.elements[p?.index];
    if(p&&hostDependent(runtime,n?.action,n))cancel();
    if(confirmation?.hostGuard)setConfirmation(null);
  },[snapshot.ui?.context,snapshot.ui?.mode,collectionContext,snapshot.hostEpoch]);
  useEffect(()=>{if(!active)cancel();const blur=()=>cancel();window.addEventListener('blur',blur);return()=>{window.removeEventListener('blur',blur);cancel();};},[active,runtime]);
  useEffect(()=>{if(!confirmation)return;const timer=setTimeout(()=>setConfirmation(null),30000);return()=>clearTimeout(timer);},[confirmation]);
  const onAction=dispatchAction?(action,node,data)=>{
    if(!active||!actionEnabled(runtime,snapshot,action)||!hostGeometryReady(runtime,snapshot,action,node))return;
    const prompt=runtime.input?.confirm?.[action];
    if(prompt){setConfirmation({action,prompt,context:snapshot.ui?.context,hostGuard:!!runtime.ownership&&hostDependent(runtime,action,node),hostEpoch:snapshot.hostEpoch});return;}
    particleActions.enqueue(action);dispatchAction(action,data?{data}:undefined,node);
  }:undefined;
  // Optional physical-key preview uses the SAME admission/confirmation path.
  const finishConfirmation=yes=>{
    const c=confirmation;setConfirmation(null);
    if(!yes||!c||!active||!actionEnabled(runtime,snapshot,c.action))return;
    if(c.hostGuard&&(snapshot.hostReply?.fresh!==true||c.hostEpoch!==snapshot.hostEpoch))return;
    particleActions.enqueue(c.action);dispatchAction(c.action,{confirm:true,context:c.context});
  };
  useLayoutEffect(()=>{
    if(!inputActionRef)return;
    const physical=action=>{
      if(confirmation){const event=bindings.find(b=>b.action===action)?.event;
        if(event==='button.sw1.short_press')finishConfirmation(true);
        else if(event==='button.sw2.short_press')finishConfirmation(false);
        return;
      }
      onAction?.(action);
    };
    inputActionRef.current=physical;return()=>{if(inputActionRef.current===physical)inputActionRef.current=null;};
  },[inputActionRef,onAction,confirmation,bindings]);
  useEffect(()=>{
    const node=svg.current;if(!node||!surface.viewport||!onAction)return;
    const wheel=e=>{const action=runtime.input?.swipe?.[e.deltaY>0?'up':'down'];if(!action||!e.deltaY||!actionEnabled(runtime,snapshot,action)||!hostGeometryReady(runtime,snapshot,action))return;e.preventDefault();if(e.timeStamp-wheelAt.current<150)return;wheelAt.current=e.timeStamp;onAction(action);};
    node.addEventListener('wheel',wheel,{passive:false});return()=>node.removeEventListener('wheel',wheel);
  },[runtime,snapshot,onAction]);
  function point(e){const b=e.currentTarget.getBoundingClientRect();return {x:(e.clientX-b.left)*640/b.width,y:(e.clientY-b.top)*480/b.height};}
  function cancel(){const p=press.current;press.current=null;if(p?.hold)onHold?.('cancel',p.action);setPressedNode(-1);}
  function down(e){
    cancel();if(confirmation||!onAction||!e.isPrimary||e.button!==0)return;
    e.currentTarget.setPointerCapture(e.pointerId);
    const q=point(e);let index=-1;
    for(let i=surface.elements.length-1;i>=0;i--){
      const n=surface.elements[i];
      if(n.action&&!n.disabled&&actionEnabled(runtime,snapshot,n.action)&&hostGeometryReady(runtime,snapshot,n.action,n)&&appVisible(n,page,snapshot.vars,stateIndex,snapshot.ui?.mode)&&hitNode(surface,n,snapshot.vars,q.x,q.y)){index=i;break;}
    }
    const action=surface.elements[index]?.action,early=pressAction(runtime,action);
    press.current={...q,id:e.pointerId,at:e.timeStamp,index,page,state:snapshot.state,scroll,editingContext,pointerEpoch:snapshot.pointerEpoch||0,collectionContext,hostContext:snapshot.ui?.context,hostMode:snapshot.ui?.mode,hostEpoch:snapshot.hostEpoch,early};
    if(onHold&&action&&action===runtime.input?.hold){
      Object.assign(press.current,{hold:true,action,allowStart:snapshot.state!==runtime.scene.active_state});
      setPressedNode(index);onHold('begin',action);return;
    }
    const v=surface.scene_view;
    if(index<0&&onDrag&&v&&snapshot.state===runtime.scene.active_state&&q.x>=v.x&&q.x<v.x+v.w&&q.y>=v.y&&q.y<v.y+v.h){
      press.current.drag=true;onDrag(q.x,q.y);
    }
    setPressedNode(index);
    if(early)onAction(action);
  }
  function up(e){
    const p=press.current,q=point(e);
    if(p?.hold&&p.id===e.pointerId){const n=surface.elements[p.index];press.current=null;setPressedNode(-1);onHold?.(n&&hitNode(surface,n,snapshot.vars,q.x,q.y)?'release':'cancel',p.action);return;}
    cancel();
    if(!onAction||!p||p.id!==e.pointerId||p.page!==page||p.state!==snapshot.state||p.scroll!==scroll||p.editingContext!==editingContext||p.pointerEpoch!==(snapshot.pointerEpoch||0)||(!runtime.ownership&&p.collectionContext!==collectionContext)||p.early)return;
    if(p.drag){onDrag?.(q.x,q.y);return;}
    const elapsed=e.timeStamp-p.at;
    const swipe=swipeAction(runtime,q.x-p.x,q.y-p.y,elapsed);
    const hostUnchanged=p.hostContext===snapshot.ui?.context&&p.hostMode===snapshot.ui?.mode&&p.hostEpoch===snapshot.hostEpoch&&p.collectionContext===collectionContext;
    const n=surface.elements[p.index];
    if(n?.value_param&&hostUnchanged&&elapsed>=0&&elapsed<10000&&Math.abs(q.y-p.y)<=28&&appVisible(n,page,snapshot.vars,stateIndex,snapshot.ui?.mode)&&actionEnabled(runtime,snapshot,n.action)&&hostGeometryReady(runtime,snapshot,n.action,n)){
      const data=sliderData(projectNode(surface,n,snapshot.vars),q.x);if(data)onAction(n.action,n,data);return;
    }
    if(swipe&&actionEnabled(runtime,snapshot,swipe)&&hostGeometryReady(runtime,snapshot,swipe)&&(!hostDependent(runtime,swipe)||hostUnchanged)){onAction(swipe);return;}
    if(!hostGeometryReady(runtime,snapshot,n?.action,n)||hostDependent(runtime,n?.action,n)&&!hostUnchanged)return;
    if(n&&!n.disabled&&elapsed>=0&&elapsed<700&&Math.abs(q.x-p.x)<=14&&Math.abs(q.y-p.y)<=14&&hitNode(surface,n,snapshot.vars,q.x,q.y)&&appVisible(n,page,snapshot.vars,stateIndex,snapshot.ui?.mode)&&actionEnabled(runtime,snapshot,n.action))onAction(n.action,n);
  }
  const gameIndex=surface.elements.findIndex(n=>n.in_view||n.view!==undefined||n.motion||n.action||n.tile||n.image_bind||n.big!==undefined||n.show||n.state_mask||n.host_mask||!['image','panel'].includes(n.type));
  const content=<>
    <ClassicGameLayer view={surface.game_view} frame={snapshot.game}/>
    <SceneSurfaceLayer view={surface.scene_view} frame={snapshot.game} images={images} clock={snapshot.clock}/>
    {runtime.scene?.engine==='p4-side-scene-v1'&&<SideSceneLayer runtime={runtime} snapshot={snapshot.side} images={images}/>}
    <DataSurfaceLayer view={surface.data_view} snapshot={snapshot.data?{...snapshot.data,rows:visibleRows(runtime,snapshot)}:snapshot.data} source={runtime.data?.source} page={snapshot.vars?.[runtime.data?.page_var]}/>
    {surface.canvas&&<CanvasLayer declaration={surface.canvas} vars={snapshot.vars} runtime={runtime} page={page}/>}
  </>;
  return <><svg ref={svg} viewBox="0 0 640 480" role="img" aria-label={`${values.title||"组件"}设备预览`}
    onPointerDown={down} onPointerUp={up} onPointerCancel={cancel} onLostPointerCapture={cancel}
    onPointerMove={e=>{const p=press.current;if(p&&p.id===e.pointerId){const q=point(e);if(p.hold){const n=surface.elements[p.index];if(!n||!hitNode(surface,n,snapshot.vars,q.x,q.y))cancel();return;}if(p.drag){if(p.state===snapshot.state)onDrag?.(q.x,q.y);else cancel();return;}if(Math.abs(q.x-p.x)>14||Math.abs(q.y-p.y)>14)setPressedNode(-1);}}}
    style={{display:"block",width:"100%",touchAction:"none",background:rgb565(surface.background)}}>
    <defs>{surfaceViewports(surface).map((v,i)=><clipPath key={i} id={`${prefix}-viewport-${i}`}><rect x={v.x} y={v.y} width={v.w} height={v.h}/></clipPath>)}</defs>
    {surface.elements.map((raw,i)=>{const n=projectNode(surface,raw,snapshot.vars);if(!n)return i===gameIndex?<g key={i}>{content}</g>:null;
      if(!appVisible(n,page,snapshot.vars,stateIndex,snapshot.ui?.mode))return i===gameIndex?<g key={i}>{content}</g>:null;
      const color=rgb565(n.color),clip=`${prefix}-${i}`,scale=n.scale||1;
      const view=nodeViewport(surface,raw),viewIndex=surfaceViewports(surface).indexOf(view);
      const nodeValue=appNodeValue(n,snapshot,values,runtime);
      const tile=appTile(n,snapshot.vars);
      const disabled=!!n.disabled||!!n.action&&(!actionEnabled(runtime,snapshot,n.action)||!hostGeometryReady(runtime,snapshot,n.action,n));
      const action=n.action&&onAction&&!disabled?()=>onAction(n.action,n,n.value_param?sliderData(n,n.x+n.w*appNodeMeter(n,snapshot,values)/100):undefined):undefined;
    return <React.Fragment key={i}>{i===gameIndex&&content}<g clipPath={view?`url(#${prefix}-viewport-${viewIndex})`:undefined}><g role={n.action?(n.value_param?"slider":"button"):undefined} aria-valuemin={n.value_param?n.value_min??0:undefined} aria-valuemax={n.value_param?n.value_max??100:undefined} aria-valuenow={n.value_param?sliderData(n,n.x+n.w*appNodeMeter(n,snapshot,values)/100)?.[n.value_param]:undefined} aria-disabled={n.action?disabled:undefined} tabIndex={action?0:undefined} aria-label={n.action?(n._collectionLabel||nodeValue||n.action):undefined}
        onClick={action?e=>{if(e.detail===0&&n.action!==runtime.input?.hold)action();}:undefined}
        onKeyDown={action?e=>{if(n.value_param&&["ArrowLeft","ArrowRight"].includes(e.key)){e.preventDefault();const percent=Math.max(0,Math.min(100,appNodeMeter(n,snapshot,values)+(e.key==="ArrowLeft"?-1:1)));const data=sliderData(n,n.x+n.w*percent/100);if(data)onAction(n.action,n,data);return;}if(e.key===" "||e.key==="Enter"){e.preventDefault();if(e.repeat)return;if(onHold&&n.action===runtime.input?.hold){if(press.current)return;press.current={hold:true,action:n.action,page,state:snapshot.state,allowStart:snapshot.state!==runtime.scene.active_state};onHold('begin',n.action);}else action();}}:undefined}
        onKeyUp={e=>{if((e.key===' '||e.key==='Enter')&&press.current?.hold){const p=press.current;press.current=null;onHold?.('release',p.action);}}}
        onBlur={()=>{if(press.current?.hold)cancel();}} style={{cursor:action?"pointer":"default",pointerEvents:action?"auto":"none"}}>
        <defs><clipPath id={clip}><rect x={n.x} y={n.y-(n.motion?4:0)} width={n.w} height={n.h+(n.motion?8:0)}/></clipPath></defs>
        <g clipPath={`url(#${clip})`}>
          {n.type==="cells"&&<CellsLayer node={n} grid={runtime.cells} cells={snapshot.cells} vars={snapshot.vars} images={images}/>}
          {(n.big!==undefined||n.type==="clip")&&<SurfaceMediaLayer node={n} surface={surface} vars={snapshot.vars} images={images} active={active}/>}
          {n.type==="image"&&n.big===undefined&&<image href={serviceImage(n,snapshot)||keyframeImage(images,surface,n,snapshot.clock)?.dataUrl} x={n.x-(tile?.index||0)*n.w} y={n.y-(tile?.row||0)*n.h+(active&&n.motion?3*Math.sin(snapshot.clock/6000*Math.PI*2):0)} width={n.w*(tile?.columns||1)} height={n.h*(tile?.rows||1)} preserveAspectRatio="none"/>}
          {(n.type==="panel"||n.type==="button")&&<><defs><linearGradient id={`${clip}-shade`} x2={n.gradient==="vertical"?"0":"1"} y2={n.gradient==="vertical"?"1":"0"}><stop stopColor={color} stopOpacity={(n.opacity??255)/255}/><stop offset="1" stopColor={color} stopOpacity={(n.opacity_end??n.opacity??255)/255}/></linearGradient></defs><rect x={n.x} y={n.y} width={n.w} height={n.h} rx={n.radius||0} fill={`url(#${clip}-shade)`}/>{n.stroke_width>0&&<rect x={n.x+n.stroke_width/2} y={n.y+n.stroke_width/2} width={n.w-n.stroke_width} height={n.h-n.stroke_width} rx={Math.max(0,(n.radius||0)-n.stroke_width/2)} fill="none" stroke={rgb565(n.stroke??n.color)} strokeWidth={n.stroke_width}/>}</>}
          {surface.graphics&&(n.type==="text"||n.type==="button")&&<GraphicLabel node={n} atlas={atlas} value={n.key_for?keyHint(n.key_for,bindings):nodeValue}/>}
          {n.type==="icon"&&<Icon name={n.icon} x={n.x} y={n.y} size={Math.min(n.w,n.h)} color={color}/>}
          {!surface.graphics&&n.type==="text"&&lines(n.bind?values[n.bind]:n.text,n.w,scale,n.h).map((line,j)=><text key={j} x={n.x} y={n.y+(j*32+16)*scale} fill={color} fontSize={16*scale} letterSpacing={scale} fontFamily="sans-serif">{line}</text>)}
          {n.type==="meter"&&<rect x={n.x} y={n.y} width={n.w*appNodeMeter(n,snapshot,values)/100} height={n.h} fill={color}/>}
        </g>
        {disabled&&<rect x={n.x} y={n.y} width={n.w} height={n.h} rx={n.radius||0} fill="#808080" opacity={.43}/>}
        {pressedNode===i&&<rect x={n.x} y={n.y} width={n.w} height={n.h} rx={n.radius||0} fill="#000" opacity={.12}/>}
        {action&&<rect x={n.x} y={n.y} width={n.w} height={n.h} fill="transparent"/>}
      </g></g></React.Fragment>;
    })}
    {gameIndex<0&&content}
    {surface.particles&&<ParticleLayer emitters={surface.particles} vars={snapshot.vars} page={page} clock={snapshot.clock} actions={particleActions} active={particleActive} generation={particleGeneration}/>}
    {confirmation&&<g role="dialog" aria-modal="true" aria-label="确认操作" onPointerDown={e=>e.stopPropagation()} onPointerUp={e=>e.stopPropagation()} onKeyDown={e=>{if(e.key==="Escape")setConfirmation(null);}}>
      <rect width="640" height="480" fill="#000" opacity=".45"/>
      <rect x="108" y="156" width="424" height="202" rx="24" fill="#fffaf2"/>
      <GraphicLabel node={{type:"text",x:136,y:180,w:368,h:76,font_size:20,line_height:28,color:12710,align:"center"}} value={confirmation.prompt}/>
      {[["取消",136,false],["确认",328,true]].map(([label,x,yes])=><g key={label} role="button" tabIndex={0} aria-label={label}
        onClick={()=>finishConfirmation(yes)}
        onKeyDown={e=>{if(e.key==="Enter"||e.key===" "){e.preventDefault();if(!e.repeat)finishConfirmation(yes);}}}>
        <rect x={x} y="276" width="176" height="56" rx="16" fill={yes?"#a36645":"#eee4d8"}/>
        <text x={x+88} y="304" dominantBaseline="central" textAnchor="middle" fontSize="20" fill={yes?"#fff":"#4d4035"}>{label}</text>
      </g>)}
    </g>}
  </svg>{visibleToast&&<div role="status" aria-live="polite">{visibleToast.bubble}{visibleToast.text?`：${visibleToast.text}`:''}</div>}</>;
}
export function ComponentAppPreview({component,active,interactive,hostSnapshot,onHostAction}){
  if(component.appRuntime?.program)return <React.Suspense fallback={<div role="status">正在准备受限程序预览</div>}><ProgramAppPreview component={component} active={active} interactive={interactive} hostSnapshot={hostSnapshot} onHostAction={onHostAction}/></React.Suspense>;
  if(component.appRuntime?.scene?.engine==='p4-side-scene-v1')return <SideAppPreview component={component} active={active} interactive={interactive}/>;
  if(component.appRuntime?.surface?.scene_view)return <SceneAppPreview component={component} active={active} interactive={interactive}/>;
  if(component.appRuntime?.surface?.game_view)return <ClassicAppPreview component={component} active={active} interactive={interactive}/>;
  return <ToolAppPreview component={component} active={active} interactive={interactive} hostSnapshot={hostSnapshot} onHostAction={onHostAction}/>;
}
function SideAppPreview({component,active,interactive}){
  const runtime=component.appRuntime,core=useRef(null),clock=useRef(0),pulse=useRef(0),held=useRef(new Set());
  const [state,setState]=useState(()=>initialApp(runtime)),[error,setError]=useState('');
  const update=()=>{if(!core.current)return;const side=core.current.snapshot();setState(s=>({...s,state:side.state,vars:{...s.vars,...side.vars},clock:side.clock,side}));};
  const stop=()=>{held.current.clear();pulse.current=0;if(core.current){core.current.axis(0);core.current.jump(false,clock.current);}};
  useEffect(()=>{let cancelled=false;core.current=null;clock.current=0;setState(initialApp(runtime));setError('');
    fetch(sideCoreUrl).then(r=>{if(!r.ok)throw Error('横版核心加载失败');return r.arrayBuffer();}).then(b=>createSideCore(b,runtime)).then(c=>{if(!cancelled){core.current=c;update();}}).catch(e=>{if(!cancelled)setError(String(e.message));});
    return()=>{cancelled=true;stop();core.current=null;};
  },[runtime]);
  useEffect(()=>{if(!active){stop();core.current?.pause(true,clock.current);return;}const timer=setInterval(()=>{if(!core.current)return;clock.current+=20;if(pulse.current&&clock.current>=pulse.current&&!held.current.size){core.current.axis(0);pulse.current=0;}core.current.tick(clock.current);update();},20);return()=>clearInterval(timer);},[active,runtime]);
  const action=a=>{if(!core.current||!actionEnabled(runtime,state,a)||!Object.values(runtime.scene.actions).includes(a))return;pulse.current=core.current.action(a,clock.current)||0;update();setState(s=>actionAccepted(runtime,{...s,clock:clock.current},a));};
  const axis=(key,down)=>{if(!core.current||!interactive)return;if(down)held.current.add(key);else held.current.delete(key);core.current.axis((held.current.has('right')?1:0)-(held.current.has('left')?1:0));update();};
  if(error)return <div role="alert">{error}</div>;
  return <div tabIndex={interactive?0:undefined} aria-label="横版闯关预览"
    onBlur={e=>{if(!e.currentTarget.contains(e.relatedTarget))stop();}}
    onKeyDown={e=>{if(!interactive||!active||e.defaultPrevented||e.target.closest?.('button,input,textarea,select,a,[role="button"]'))return;if(e.key==='ArrowLeft'||e.key==='ArrowRight'){e.preventDefault();axis(e.key==='ArrowLeft'?'left':'right',true);}else if((e.key===' '||e.key==='Enter')&&!e.repeat){e.preventDefault();core.current?.jump(true,clock.current);update();}else if(e.key==='p'&&!e.repeat)action(runtime.scene.actions.pause);}}
    onKeyUp={e=>{if(e.key==='ArrowLeft'||e.key==='ArrowRight')axis(e.key==='ArrowLeft'?'left':'right',false);if(e.key===' '||e.key==='Enter'){core.current?.jump(false,clock.current);update();}}}>
    <AppSurface componentId={component.id} runtime={runtime} snapshot={state} images={component.sceneSprites} bindings={component.defaultBindings} active={active} onAction={interactive?action:undefined} onHold={interactive?(phase)=>{core.current?.jump(phase==='begin',clock.current);update();}:undefined}/>
    {interactive&&<div aria-label="横版控制"><button onClick={e=>{if(!e.detail)action(runtime.scene.actions.left);}} onPointerDown={e=>{e.currentTarget.setPointerCapture(e.pointerId);axis('left',true);}} onPointerUp={()=>axis('left',false)} onPointerCancel={()=>axis('left',false)} onLostPointerCapture={()=>axis('left',false)}>←</button><button onClick={e=>{if(!e.detail)action(runtime.scene.actions.right);}} onPointerDown={e=>{e.currentTarget.setPointerCapture(e.pointerId);axis('right',true);}} onPointerUp={()=>axis('right',false)} onPointerCancel={()=>axis('right',false)} onLostPointerCapture={()=>axis('right',false)}>→</button><button onClick={e=>{if(!e.detail)action(runtime.scene.actions.jump);}} onPointerDown={e=>{e.currentTarget.setPointerCapture(e.pointerId);core.current?.jump(true,clock.current);update();}} onPointerUp={()=>{core.current?.jump(false,clock.current);update();}} onPointerCancel={stop} onLostPointerCapture={()=>{core.current?.jump(false,clock.current);}}>跳跃</button><button onClick={()=>action(runtime.scene.actions.pause)}>暂停 / 继续</button><small>聚焦预览后：方向键移动，空格跳跃，P 暂停。</small></div>}
  </div>;
}
function SceneAppPreview({component,active,interactive}){
  const runtime=component.appRuntime,core=useRef(null),current=useRef(initialApp(runtime));
  const confirming=useRef(false);
  const confirmationChanged=useCallback(open=>{
    confirming.current=open;
    if(open&&core.current&&runtime.scene.motion)core.current.release(true,current.current.clock);
  },[runtime]);
  const [state,setState]=useState(current.current),[error,setError]=useState('');
  function update(s){const g=core.current.snapshot(),next={...s,game:g,vars:{...s.vars,...(runtime.logic?core.current.readVars():{})}};
    if(g.over&&runtime.scene.result_state)next.state=runtime.scene.result_state;
    if(runtime.scene.score_var)next.vars[runtime.scene.score_var]=g.score;
    if(runtime.scene.motion?.charge_var)next.vars[runtime.scene.motion.charge_var]=g.charge;
    if(runtime.lifecycle?.record_var)next.vars[runtime.lifecycle.record_var]=Math.max(next.vars[runtime.lifecycle.record_var]||0,g.score);
    current.current=next;setState(next);
  }
  useEffect(()=>{let cancelled=false;core.current=null;current.current=initialApp(runtime);setState(current.current);setError('');
    fetch(classicUrl).then(r=>{if(!r.ok)throw Error('游戏引擎加载失败');return r.arrayBuffer();}).then(b=>cancelled?null:createSceneCore(b,runtime.scene,runtime.logic?runtime.vars:undefined,runtime.sfx?.cues?.map(c=>c.id))).then(c=>{if(!cancelled){core.current=c;update(current.current);}}).catch(e=>{if(!cancelled)setError(e.message);});
    return()=>{cancelled=true;core.current=null;};
  },[runtime]);
  useEffect(()=>{if(!active)return;const ms=runtime.scene.motion?20:50;const timer=setInterval(()=>{if(core.current){const next=appAdvance(runtime,current.current,ms);if(runtime.logic)core.current.setVars(next.vars);if(next.state===runtime.scene.active_state&&!((runtime.scene.motion||runtime.scene.bodies)&&confirming.current))core.current.tick(next.clock);else core.current.hold(next.clock);update(next);}},ms);return()=>clearInterval(timer);},[runtime,active]);
  if(error)return <div role="alert">{error}</div>;
  const action=a=>{if(!core.current||!actionEnabled(runtime,current.current,a)||!runtime.transitions.some(t=>t.on===a&&(t.from==='*'||t.from===current.current.state)))return;
    const next=appAction(runtime,current.current,a);
    const resuming=a===runtime.lifecycle?.pause_action&&next.state===runtime.scene.active_state&&current.current.state!==next.state&&runtime.transitions.some(t=>t.on===runtime.lifecycle?.pause_action&&t.from===current.current.state&&t.to===next.state);
    if(next.state!==current.current.state){core.current.release(true,next.clock);core.current.hold(next.clock);}
    if(runtime.logic)core.current.setVars(next.vars);
    if(!resuming)core.current.action(a,next.clock);
    update(next);
  };
  const drag=(x,y)=>{
    const d=runtime.input?.drag,v=runtime.surface.scene_view;
    if(!core.current||!d||current.current.state!==runtime.scene.active_state)return;
    const g=core.current.snapshot(),cell=Math.min(Math.floor(v.w/g.width),Math.floor(v.h/g.height));
    const ox=v.x+(v.w-cell*g.width)/2,oy=v.y+(v.h-cell*g.height)/2;
    core.current.pointer(d.entity,Math.trunc((x-ox)/cell),Math.trunc((y-oy)/cell),{x:1,y:2,xy:3}[d.axis]);update(current.current);
  };
  const hold=(phase,a)=>{if(!core.current)return;if(phase==='begin')action(a);else{core.current.release(phase!=='release',current.current.clock);update(current.current);}};
  return <AppSurface componentId={component.id} runtime={runtime} snapshot={state} images={component.sceneSprites} bindings={component.defaultBindings} active={active} onAction={interactive?action:undefined} onHold={interactive&&runtime.input?.hold?hold:undefined} onDrag={interactive&&runtime.input?.drag?drag:undefined} onConfirmationChange={confirmationChanged}/>;
}
function ClassicAppPreview({component,active,interactive}){
  const runtime=component.appRuntime,core=useRef(null),clock=useRef(0);
  const [state,setState]=useState(()=>initialApp(runtime)),[error,setError]=useState('');
  // Runtime tick[] keeps running beside a native preset on the device; mirror it here (ms=0 for action refreshes).
  const refresh=(ms=0)=>{const g=core.current.snapshot();setState(s=>{const a=ms?appAdvance(runtime,{...s,state:g.state||runtime.initial_state},ms):s;return {...a,game:g,state:g.state||runtime.initial_state,vars:{...a.vars,[runtime.game.score_var]:g.score,...(runtime.lifecycle?.record_var?{[runtime.lifecycle.record_var]:g.record}:{})}};});};
  useEffect(()=>{let cancelled=false;core.current=null;clock.current=0;setError('');setState(initialApp(runtime));fetch(classicUrl).then(r=>{if(!r.ok)throw Error('游戏引擎加载失败');return r.arrayBuffer();}).then(b=>createClassicCore(b,runtime)).then(c=>{if(!cancelled){core.current=c;refresh();}}).catch(e=>{if(!cancelled)setError(String(e.message));});return()=>{cancelled=true;core.current=null;};},[runtime]);
  useEffect(()=>{if(!active)return;const timer=setInterval(()=>{if(core.current){clock.current+=50;core.current.tick(clock.current);refresh(50);}},50);return()=>clearInterval(timer);},[active,runtime]);
  if(error)return <div role="alert">{error}</div>;
  return <AppSurface componentId={component.id} runtime={runtime} snapshot={state} images={component.sceneSprites} bindings={component.defaultBindings} active={active} onAction={interactive?a=>{if(core.current){core.current.action(a,clock.current);refresh();}}:undefined}/>;
}
function ToolAppPreview({component,active,interactive,hostSnapshot,onHostAction}){
  const runtime=component.appRuntime;const [state,setState]=useState(()=>runtime?initialApp(runtime):null);
  const inputAction=useRef(null),[confirming,setConfirming]=useState(false);
  const [data,setData]=useState(null);
  useEffect(()=>{
    setData(null);
    if(!active||!interactive||!runtime?.data||!window.__TAURI_INTERNALS__)return;
    let stopped=false,timer;
    async function poll(){try{const d=await invoke('local_widget_snapshot',{source:runtime.data.source});if(!stopped)setData(d);}catch{if(!stopped)setData(d=>({...d,stale:true,status:'error',message:'数据暂不可用'}));}if(!stopped)timer=setTimeout(poll,2000);}
    poll();return()=>{stopped=true;clearTimeout(timer);};
  },[runtime,active,interactive]);
  useEffect(()=>{setState(runtime?initialApp(runtime):null);},[runtime,component.versionHash]);
  useEffect(()=>{
    if(!runtime?.form)return;
    const discard=()=>setState(s=>s?formCancel(runtime,structuredClone(s)):s);
    if(!active)discard();
    const visibility=()=>{if(document.hidden)discard();};
    window.addEventListener('blur',discard);document.addEventListener('visibilitychange',visibility);
    return()=>{window.removeEventListener('blur',discard);document.removeEventListener('visibilitychange',visibility);};
  },[runtime,active]);
  useEffect(()=>{if(!active||!runtime)return;const timer=setInterval(()=>setState(s=>s?appAdvance(runtime,s,100):s),100);return()=>clearInterval(timer);},[active,runtime]);
  if(!runtime||!state)return <div role="status">组件预览数据未就绪</div>;
  const presented=mixedHostSnapshot(runtime,{...state,data},hostSnapshot);
  if((runtime.host&&!runtime.ownership||component.pcApplication?.engine==='pc-fetch-v1')&&hostSnapshot){presented.vars={...presented.vars,...hostSnapshot.variables};presented.values=hostSnapshot.values;presented.ui=hostSnapshot.ui;presented.data=hostSnapshot.data;}
  const dispatch=(a,confirmation,node)=>{if(!active||!actionEnabled(runtime,presented,a))return;if(hostAction(runtime,a)||component.pcApplication?.engine==='pc-fetch-v1'&&Object.hasOwn(component.pcApplication.actions||{},a)){onHostAction?.(a,confirmation);return;}setState(s=>dAction(runtime,s,a,node)||appAction(runtime,dSnapshot(runtime,s),a));};
  const view=<><AppSurface componentId={component.id} runtime={runtime} snapshot={presented} active={active} images={component.sceneSprites} bindings={component.defaultBindings} inputActionRef={inputAction} onConfirmationChange={setConfirming} onAction={interactive&&(!runtime.host||runtime.ownership)?dispatch:undefined}/></>;
  if(!runtime.form)return view;
  // Expose the actual declared mappings, including forms with no touch nodes.
  // Native buttons are keyboard accessible; no SW key is assigned a form role.
  return <div aria-label="有界表单预览">{view}{interactive&&[MIXED_FORM_ENGINE,TEXT_FORM_ENGINE].includes(runtime.form.engine)&&<FormTextEditor key={`${runtime.form.engine}:${state.vars[runtime.form.focus_var]}`} runtime={runtime} snapshot={state} enabled={active}
    onReplace={(field,text,context)=>setState(s=>{if(formContext(runtime,s)!==context)return s;const next=structuredClone(s);return (runtime.form.engine===TEXT_FORM_ENGINE?replaceTextDraft:replaceMixedDraft)(runtime,next,field,text)==='applied'?next:s;})}/>}{interactive&&<div role="group" aria-label="表单实体按键预览">
    {(component.defaultBindings||[]).filter(b=>formOperation(runtime,b.action)).map(b=><button key={b.action} type="button"
      disabled={!active||(confirming?!['button.sw1.short_press','button.sw2.short_press'].includes(b.event):!actionEnabled(runtime,presented,b.action))} title={b.event} onClick={()=>inputAction.current?.(b.action)}>{b.label||b.action}</button>)}
  </div>}</div>;
}
