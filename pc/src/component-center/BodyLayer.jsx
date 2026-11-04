import React,{useEffect,useId,useState} from 'react';
import {createBodiesPreview} from './bodies-core.js';
import coreUrl from './classic-core.wasm?url';
let bytes;
const load=()=>bytes??=(fetch(coreUrl).then(r=>{if(!r.ok)throw Error('Bodies core unavailable');return r.arrayBuffer();}).catch(e=>{bytes=null;throw e;}));
/** Optional standalone view. App scene previews already use this same C/WASM path. */
export default function BodyLayer({scene,view,clock=0,actions,vars,cues}){
  const prefix=useId().replaceAll(':',''),[owned,setOwned]=useState(null),[frame,setFrame]=useState(null);
  useEffect(()=>{let live=true;setOwned(null);setFrame(null);
    load().then(b=>createBodiesPreview(b,scene,vars,cues)).then(core=>{if(live)setOwned({scene,core});}).catch(error=>{if(live)setOwned({scene,error});});
    return()=>{live=false;};
  },[scene,vars,cues]);
  useEffect(()=>{
    if(owned?.scene!==scene||!owned.core)return;
    try{for(const action of actions?.current||[])owned.core.action(action,clock);if(actions)actions.current=[];
      owned.core.tick(clock);setFrame({owned,snapshot:owned.core.snapshot()});
    }catch(error){setFrame({owned,error});}
  },[owned,scene,clock,actions]);
  if(owned?.error||frame?.owned===owned&&frame?.error)return <g data-bodies="invalid"/>;
  if(!view||!owned||frame?.owned!==owned)return <g data-bodies="loading"/>;
  const s=frame.snapshot,cell=Math.min(view.w/s.width,view.h/s.height),ox=view.x+(view.w-s.width*cell)/2,oy=view.y+(view.h-s.height*cell)/2;
  return <g data-bodies="shared-core" pointerEvents="none"><defs><clipPath id={`${prefix}-bodies`}><rect x={view.x} y={view.y} width={view.w} height={view.h}/></clipPath></defs>
    <g clipPath={`url(#${prefix}-bodies)`}>{s.entities.map(e=>{const x=ox+e.x*cell,y=oy+e.y*cell,w=e.width*cell,h=e.height*cell;
      return <rect key={e.id} data-entity={e.id} x={x} y={y} width={w} height={h} fill="currentColor" transform={`rotate(${e.rotation||0} ${x+w/2} ${y+h/2})`}/>;
    })}</g></g>;
}
