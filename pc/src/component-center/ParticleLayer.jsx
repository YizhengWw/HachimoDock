import React,{useEffect,useId,useState} from 'react';
import {createParticleCore} from './canvas-core.js';
import {stepParticlePreview} from './particle-preview.js';
import coreUrl from './canvas-core.wasm?url';
import {rgb565} from './media-surface-model.js';
const EMPTY_EMITTERS=[];
let bytes;
function load(){return bytes||=(fetch(coreUrl).then(r=>{if(!r.ok)throw Error('particle_core_unavailable');return r.arrayBuffer();}).catch(error=>{bytes=null;throw error;}));}
/** p4-particles-v1 preview: the same integer core as the device, drawn above every node, never cached. */
export default function ParticleLayer({emitters=EMPTY_EMITTERS,vars,page,clock=0,actions,active=false,generation=0}){
  const prefix=useId().replaceAll(':','');
  const [owned,setOwned]=useState(null);const [dots,setDots]=useState([]);
  useEffect(()=>{let live=true;setOwned(null);setDots([]);
    actions?.setReady(emitters,false,generation);
    if(!active||!emitters.length)return()=>{live=false;};
    load().then(b=>createParticleCore(b)).then(core=>{if(!live||!actions?.isCurrent(emitters,generation))return;emitters.forEach((e,i)=>core.begin(i,e,0x9e3779b9+i));actions?.setReady(emitters,true,generation);setOwned({emitters,actions,generation,core});}).catch(()=>{if(live)setOwned({emitters,actions,generation,error:true});});
    return()=>{live=false;actions?.setReady(emitters,false,generation);};
  },[emitters,actions,active,generation]);
  useEffect(()=>{
    const core=active&&owned?.emitters===emitters&&owned.actions===actions&&owned.generation===generation&&actions?.isReady(emitters,generation)?owned.core:null;if(!core)return;
    const pending=actions?.take(emitters,generation)||[];
    setDots(stepParticlePreview(core,emitters,{vars,page,clock,actions:pending}));
  },[owned,emitters,actions,active,generation,clock,page,vars]);
  if(!active||!emitters?.length)return null;
  const current=owned?.emitters===emitters&&owned.actions===actions&&owned.generation===generation;
  return <g data-particles={current&&owned.error?'error':current&&owned.core?'ready':'loading'} pointerEvents="none">
    <defs>{emitters.map((e,i)=><clipPath key={i} id={`${prefix}-emitter-${i}`}><rect x={e.x} y={e.y} width={e.w} height={e.h}/></clipPath>)}</defs>
    {(current&&actions?.isReady(emitters,generation)?dots:[]).map(([x,y,size,color,i],k)=><g key={k} clipPath={`url(#${prefix}-emitter-${i})`} fill={rgb565(color)}>{size===1?<rect x={x} y={y} width="1" height="1"/>:<><rect x={x-1} y={y} width="3" height="1"/><rect x={x} y={y-1} width="1" height="1"/><rect x={x} y={y+1} width="1" height="1"/></>}</g>)}
  </g>;
}
