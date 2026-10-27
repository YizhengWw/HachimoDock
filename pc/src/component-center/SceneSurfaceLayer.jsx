import React,{useId} from 'react';
import {rgb565} from './media-surface-model.js';
export default function SceneSurfaceLayer({view:v,frame:g,images,clock=0}){
  const prefix=useId().replaceAll(':','');
  if(!v||!g?.width||!g?.height)return null;
  const cell=Math.min(Math.floor(v.w/g.width),Math.floor(v.h/g.height)),ox=v.x+(v.w-cell*g.width)/2,oy=v.y+(v.h-cell*g.height)/2;
  const colors=v.colors.map(rgb565);
  return <g data-content-view="scene"><defs><clipPath id={`${prefix}-view`}><rect x={v.x} y={v.y} width={v.w} height={v.h}/></clipPath></defs><g clipPath={`url(#${prefix}-view)`}>{g.entities.filter(e=>e.active).map(e=>{
    const x=ox+e.x*cell,y=oy+e.y*cell,w=e.width*cell,h=e.height*cell,sp=v.sprites.find(s=>s.entity===e.id);
    const color=colors[[0,1,2,0,3][Math.max(1,Math.min(4,e.tone))]],clip=`${prefix}-${e.id}`;
    if(sp){const image=images.find(im=>im.id===sp.image),frame=Math.floor(clock*sp.fps/1000)%sp.columns;return <g key={e.id} data-entity={e.id} data-x={e.x} data-y={e.y}><defs><clipPath id={clip}><rect x={x} y={y} width={w} height={h}/></clipPath></defs><image href={image?.dataUrl} x={x-frame*w} y={y} width={w*sp.columns} height={h} preserveAspectRatio="none" clipPath={`url(#${clip})`}/></g>;}
    return <g key={e.id} data-entity={e.id} data-x={e.x} data-y={e.y} data-width={e.width} fill={color}>
      {['ball','circle','coin'].includes(e.shape)?<ellipse cx={x+w/2} cy={y+h/2} rx={w/2-2} ry={h/2-2}/>:e.shape==='heart'?<path d={`M ${x+w/2} ${y+h-2} C ${x-w*.1} ${y+h*.55} ${x} ${y-h*.1} ${x+w/2} ${y+h*.3} C ${x+w} ${y-h*.1} ${x+w*1.1} ${y+h*.55} ${x+w/2} ${y+h-2}`}/>:<rect x={x+2} y={y+2} width={w-4} height={h-4} rx={Math.min(w,h)/3}/>}
    </g>;
  })}</g></g>;
}
