import React,{useId} from 'react';
import {rgb565} from './media-surface-model.js';
/** Draw commands come from the firmware core, including clipping/camera/skin. */
export default function SideSceneLayer({runtime,snapshot,images=[]}){
  const id=useId().replaceAll(':',''),a=runtime.scene?.art;
  if(!a||!snapshot)return null;
  const image=images.find(im=>im.id===a.image);
  if(!image)return null;
  return <g data-content-view="side-scene"><defs><clipPath id={id}><rect x={a.x} y={a.y} width={a.w} height={a.h}/></clipPath></defs><g clipPath={`url(#${id})`}>
    <rect x={a.x} y={a.y} width={a.w} height={a.h} fill={rgb565(a.sky)}/>
    {snapshot.commands.map(([x,y,w,h,frame,flip],i)=>{const sx=frame%a.columns*16,sy=Math.floor(frame/a.columns)*16;return <svg key={i} x={x} y={y} width={w} height={h} viewBox={`${sx} ${sy} 16 16`} preserveAspectRatio="none" overflow="hidden"><image href={image.dataUrl} width={a.columns*16} height={a.rows*16} transform={flip?`translate(${sx*2+16} 0) scale(-1 1)`:undefined} style={{imageRendering:'pixelated'}}/></svg>;})}
  </g></g>;
}
