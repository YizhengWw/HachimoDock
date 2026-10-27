import React, {useEffect,useId,useState,useRef} from "react";
import {invoke} from "@tauri-apps/api/core";
import {EMPTY_MEDIA,mediaBindings,mediaQueue,rgb565} from "./media-surface-model.js";

export function MediaSurface({surface,snapshot=EMPTY_MEDIA,command,images=[]}) {
  const id=useId().replaceAll(":","");const s={...EMPTY_MEDIA,...snapshot},values=mediaBindings(s);
  const queue=mediaQueue(s),index=Math.max(0,queue.findIndex(t=>t.id===s.current?.id));
  const queueKey=JSON.stringify([s.source,queue.map(t=>t.id)]);
  const [local,setLocal]=useState({page:null,selected:null});
  const pointer=useRef(null),lastSelect=useRef(-Infinity);
  const [dragVolume,setDragVolume]=useState(null);
  const selected=Math.max(0,Math.min(queue.length-1,local.selected??s.selected??index));
  const page=local.page??s.page??0,playing=["playing","buffering"].includes(s.state);
  useEffect(()=>{pointer.current=null;setDragVolume(null);},[surface,page,queueKey]);
  function point(e){
    const box=e.currentTarget.ownerSVGElement.getBoundingClientRect();
    return {x:(e.clientX-box.left)*640/Math.max(1,box.width),y:(e.clientY-box.top)*480/Math.max(1,box.height)};
  }
  function select(index){
    if(!command||!queue[index]||performance.now()-lastSelect.current<300)return;
    lastSelect.current=performance.now();setLocal({page:0,selected:index});command({operation:"select",id:queue[index].id});
  }
  function act(action){
    if(!command)return;
    if(action==="media.queue"){setLocal({page:page===1?0:1,selected});return;}
    if(page===1&&["media.up","media.down"].includes(action)){
      setLocal({page,selected:queue.length?(selected+queue.length+(action==="media.up"?-1:1))%queue.length:0});return;
    }
    if(action==="media.select"||action==="media.toggle"&&page===1){select(selected);return;}
    const operation={"media.toggle":"toggle","media.previous":"previous","media.next":"next","media.up":"volume_up","media.down":"volume_down"}[action];
    if(operation)command({operation});
  }
  const [elapsed,setElapsed]=useState(0);
  useEffect(()=>{
    if(s.state!=="playing")return;
    let frame,last=performance.now();
    const tick=now=>{setElapsed(t=>t+Math.min(100,now-last));last=now;frame=requestAnimationFrame(tick);};
    frame=requestAnimationFrame(tick);return()=>cancelAnimationFrame(frame);
  },[s.state]);
  return <svg viewBox="0 0 640 480" role="img" aria-label="组件设备预览" style={{display:"block",width:"100%",borderRadius:20,background:rgb565(surface?.background)}}>
    {(surface?.elements||[]).filter(n=>n.page==null||n.page===2||n.page===page).map((n,i)=>{
      const color=rgb565(n.color),bg=rgb565(surface.background),clip=`${id}-${i}`;
      return <g key={i} role={n.action&&command?"button":undefined} tabIndex={n.action&&command?0:undefined} aria-label={n.action?(n.type==="transport"?(playing?"停止播放":"开始播放"):(n.text||n.action)):undefined} onClick={()=>n.action&&act(n.action)} onKeyDown={e=>{if(n.action&&command&&["Enter"," "].includes(e.key)){e.preventDefault();act(n.action);}}} style={{cursor:n.action&&command?"pointer":undefined}}><defs><clipPath id={clip}><rect x={n.x} y={n.y} width={n.w} height={n.h}/></clipPath></defs><g clipPath={`url(#${clip})`}>
        {n.action&&<rect x={n.x} y={n.y} width={n.w} height={n.h} fill="transparent"/>}
        {n.type==="image"&&(()=>{const im=images.find(im=>im.id===n.image);return im?<image href={im.dataUrl} x={n.x} y={n.y} width={n.w} height={n.h} preserveAspectRatio="none" transform={n.rotate_ms?`rotate(${elapsed%n.rotate_ms/n.rotate_ms*360} ${n.x+n.w/2} ${n.y+n.h/2})`:undefined}/>:null;})()}
        {n.type==="panel"&&<rect x={n.x} y={n.y} width={n.w} height={n.h} rx={n.radius||0} fill={color}/>}
        {n.type==="text"&&<text x={n.x} y={n.y+15*(n.scale||1)} fill={color} fontSize={16*(n.scale||1)} letterSpacing={n.scale||1} fontFamily="sans-serif">{n.bind?values[n.bind]:n.text}</text>}
        {n.type==="meter"&&<rect x={n.x} y={n.y} width={n.w*Math.min(100,Math.max(0,n.bind==="volume"?(dragVolume??s.volume):s.level||0))/100} height={n.h} fill={color}/>}
        {n.type==="ticks"&&<>{Array.from({length:41},(_,j)=><rect key={j} x={n.x+j*(n.w-2)/40} y={n.y+n.h-(j%5?n.h/3:n.h/2)} width={1} height={j%5?n.h/3:n.h/2} fill={color}/>)}{queue.length>0&&<rect x={n.x+(queue.length>1?index*(n.w-2)/(queue.length-1):(n.w-2)/2)} y={n.y} width={2} height={n.h} fill={color}/>}</>}
        {n.type==="list"&&(()=>{const rows=Math.max(1,Math.floor(n.h/48)),first=Math.floor(selected/rows)*rows;return queue.slice(first,first+rows).map((t,j)=><g key={t.id} role={command?"button":undefined} tabIndex={command?0:undefined} aria-label={`收听 ${t.name}`}
          onPointerDown={e=>{e.stopPropagation();if(e.isPrimary&&e.button===0){e.currentTarget.setPointerCapture(e.pointerId);pointer.current={id:e.pointerId,...point(e),at:e.timeStamp,key:t.id,queueKey};}}}
          onPointerCancel={()=>{pointer.current=null;}}
          onPointerUp={e=>{e.stopPropagation();const p=pointer.current,q=point(e);pointer.current=null;if(p&&p.id===e.pointerId&&p.queueKey===queueKey&&p.key===t.id&&e.timeStamp-p.at<700&&Math.abs(q.x-p.x)<=14&&Math.abs(q.y-p.y)<=14)select(first+j);}}
          onClick={e=>{e.stopPropagation();if(e.detail===0)select(first+j);}} onKeyDown={e=>{if(["Enter"," "].includes(e.key)){e.preventDefault();e.stopPropagation();select(first+j);}}}>
          <rect x={n.x} y={n.y+j*48+2} width={n.w} height={44} rx={10} fill={first+j===selected?color:"transparent"}/>
          <text x={n.x+16} y={n.y+j*48+23} fill={first+j===selected?bg:color} fontSize={16} letterSpacing={1} fontFamily="sans-serif">{t.name}</text>
        </g>);})()}
        {n.type==="transport"&&<g role={command&&!n.action?"button":undefined} tabIndex={command&&!n.action?0:undefined} aria-label={playing?"停止播放":"开始播放"} onClick={e=>{e.stopPropagation();command?.({operation:"toggle"});}} onKeyDown={e=>{if(command&&(e.key==="Enter"||e.key===" ")){e.stopPropagation();e.preventDefault();command({operation:"toggle"});}}} style={{cursor:command?"pointer":"default"}}><circle cx={n.x+n.w/2} cy={n.y+n.h/2} r={Math.min(n.w,n.h)/2} fill={color}/>{playing?<rect x={n.x+n.w/3} y={n.y+n.h/3} width={n.w/3} height={n.h/3} fill={bg}/>:<path d={`M${n.x+n.w/3} ${n.y+n.h/4}L${n.x+n.w*2/3} ${n.y+n.h/2}L${n.x+n.w/3} ${n.y+n.h*3/4}Z`} fill={bg}/>}</g>}
      </g>{n.type==="meter"&&n.bind==="volume"&&command&&<rect x={n.x} y={n.y-20} width={n.w} height={n.h+40} fill="transparent" role="slider" tabIndex={0} aria-label="音量" aria-valuemin={0} aria-valuemax={100} aria-valuenow={dragVolume??s.volume}
        onPointerDown={e=>{e.stopPropagation();if(!e.isPrimary||e.button!==0)return;e.currentTarget.setPointerCapture(e.pointerId);pointer.current={id:e.pointerId,y:point(e).y,at:e.timeStamp,queueKey};const box=e.currentTarget.getBoundingClientRect();setDragVolume(Math.round(Math.max(0,Math.min(100,(e.clientX-box.left)/box.width*100))));}}
        onPointerMove={e=>{if(pointer.current?.id!==e.pointerId)return;const box=e.currentTarget.getBoundingClientRect();setDragVolume(Math.round(Math.max(0,Math.min(100,(e.clientX-box.left)/box.width*100))));}}
        onPointerCancel={()=>{pointer.current=null;setDragVolume(null);}}
        onPointerUp={e=>{e.stopPropagation();const p=pointer.current;pointer.current=null;setDragVolume(null);if(!p||p.id!==e.pointerId||p.queueKey!==queueKey||e.timeStamp-p.at>=10000||Math.abs(point(e).y-p.y)>28)return;const box=e.currentTarget.getBoundingClientRect();command({operation:"volume",volume:Math.round(Math.max(0,Math.min(100,(e.clientX-box.left)/box.width*100)))});}}
        onKeyDown={e=>{if(["ArrowLeft","ArrowRight"].includes(e.key)){e.preventDefault();e.stopPropagation();command({operation:"volume",volume:Math.max(0,Math.min(100,s.volume+(e.key==="ArrowLeft"?-5:5)))});}}}
      />}</g>;
    })}
  </svg>;
}
export function ComponentMediaPreview({component,active}) {
  const [snapshot,setSnapshot]=useState(EMPTY_MEDIA);
  useEffect(()=>{
    setSnapshot(EMPTY_MEDIA);if(!active||!window.__TAURI_INTERNALS__)return;
    let stopped=false,timer;
    async function poll(){try{const s=await invoke("component_extension_request",{componentId:component.id,input:{operation:"status"}});if(!stopped)setSnapshot(s);}catch(e){if(!stopped)setSnapshot({...EMPTY_MEDIA,state:"error",message:String(e)});}if(!stopped)timer=setTimeout(poll,1500);}
    poll();return()=>{stopped=true;clearTimeout(timer);};
  },[component.id,component.versionHash,active]);
  return <MediaSurface surface={component.mediaSurface} snapshot={snapshot} images={component.sceneSprites}/>;
}
