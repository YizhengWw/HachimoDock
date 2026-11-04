import React,{useEffect,useLayoutEffect,useMemo,useRef,useState} from 'react';
import {rgb565} from './media-surface-model.js';

const png=/^data:image\/png;base64,[A-Za-z0-9+/]+={0,2}$/;
const mp4=/^data:video\/mp4;base64,[A-Za-z0-9+/]+={0,2}$/;
const integerOffset=(value,max)=>Math.max(0,Math.min(max,Number.isInteger(value)?value:0));

function Unavailable({node:n,message='预览不可用'}){
  return <g><rect x={n.x} y={n.y} width={n.w} height={n.h} fill={rgb565(n.color)}/>
    {message&&<text x={n.x+n.w/2} y={n.y+n.h/2} textAnchor="middle" dominantBaseline="central"
      fontSize={14} fill="#fff" stroke="#000" strokeWidth={.5} paintOrder="stroke">{message}</text>}
  </g>;
}

function BigImage({node:n,surface,vars,images}){
  const image=images.find(im=>im.id===`big:${n.big}`),source=image?.dataUrl;
  const declaration=surface.big_images?.find(im=>im.id===n.big);
  const width=declaration?.width,height=declaration?.height;
  const [failedSource,setFailedSource]=useState(null);
  const valid=useMemo(()=>typeof source==='string'&&png.test(source),[source]);
  if(!valid||failedSource===source||!Number.isInteger(width)||!Number.isInteger(height)
    ||width<n.w||height<n.h||width>1280||height>960
    ||(image.frameWidth??image.frame_width)!==width||(image.frameHeight??image.frame_height)!==height){
    return <Unavailable node={n}/>;
  }
  const ox=integerOffset(vars[n.offset_x_var],width-n.w),oy=integerOffset(vars[n.offset_y_var],height-n.h);
  return <image href={source} x={n.x-ox} y={n.y-oy} width={width} height={height}
    preserveAspectRatio="none" onError={()=>setFailedSource(source)}/>;
}

/** Original package MP4 only: browser timing/decoding is not the device H.264 path. */
function Clip({node:n,source,active}){
  const video=useRef(null),[failed,setFailed]=useState(false);
  const [documentVisible,setDocumentVisible]=useState(()=>typeof document==='undefined'||!document.hidden);
  const valid=useMemo(()=>typeof source==='string'&&mp4.test(source),[source]);
  const enabled=active&&documentVisible&&valid;
  useEffect(()=>{
    const changed=()=>setDocumentVisible(!document.hidden);
    document.addEventListener('visibilitychange',changed);changed();
    return()=>document.removeEventListener('visibilitychange',changed);
  },[]);
  useLayoutEffect(()=>{
    const element=video.current;if(!enabled||!element)return;
    let stopped=false,frame;
    setFailed(false);
    const release=()=>{element.pause();element.removeAttribute('src');element.load();};
    const fail=()=>{
      if(stopped)return;stopped=true;cancelAnimationFrame(frame);release();setFailed(true);
    };
    const play=()=>{
      if(stopped)return;
      try{element.play()?.catch(fail);}catch{fail();}
    };
    const restart=()=>{
      if(stopped)return;
      try{element.currentTime=0;play();}catch{fail();}
    };
    const advance=()=>{
      if(stopped)return;
      if(element.currentTime>=5&&!element.seeking)restart();
      if(!stopped)frame=requestAnimationFrame(advance);
    };
    element.addEventListener('error',fail);
    element.addEventListener('ended',restart);
    element.muted=true;element.defaultMuted=true;element.playsInline=true;
    element.src=source;play();if(!stopped)frame=requestAnimationFrame(advance);
    return()=>{
      stopped=true;cancelAnimationFrame(frame);
      element.removeEventListener('error',fail);element.removeEventListener('ended',restart);
      release();
    };
  },[enabled,source]);
  return <>
    <Unavailable node={n} message={active&&(!valid||failed)?'预览不可用':''}/>
    {enabled&&<foreignObject x={n.x} y={n.y} width={n.w} height={n.h} style={{pointerEvents:'none'}}>
      <div xmlns="http://www.w3.org/1999/xhtml" style={{width:'100%',height:'100%',background:failed?'transparent':'#000'}}>
        <video ref={video} muted playsInline preload="none" controls={false} tabIndex={-1} aria-hidden="true"
          disablePictureInPicture disableRemotePlayback style={{display:failed?'none':'block',width:'100%',height:'100%',objectFit:'contain',background:'#000'}}/>
      </div>
    </foreignObject>}
  </>;
}

export default function SurfaceMediaLayer({node,surface,vars={},images=[],active=false}){
  if(node.big!==undefined)return <BigImage key={node.big} node={node} surface={surface} vars={vars} images={images}/>;
  if(node.type==='clip')return <Clip key={node.asset} node={node} source={images.find(im=>im.id===`clip:${node.asset}`)?.dataUrl} active={active}/>;
  return null;
}
