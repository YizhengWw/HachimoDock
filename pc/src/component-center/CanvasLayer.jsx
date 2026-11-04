import React,{useEffect,useMemo,useState} from 'react';
import {createCanvasCore} from './canvas-core.js';
import coreUrl from './canvas-core.wasm?url';
import {rgb565} from './media-surface-model.js';
let bytes;
function load(){return bytes||=(fetch(coreUrl).then(r=>{if(!r.ok)throw Error('canvas_core_unavailable');return r.arrayBuffer();}).catch(error=>{bytes=null;throw error;}));}
export default function CanvasLayer({declaration,vars,runtime,page=0}){
  const [owned,setOwned]=useState(null);
  useEffect(()=>{let current=true;if(!declaration){setOwned(null);return;}
    load().then(b=>createCanvasCore(b,declaration,runtime)).then(core=>{if(current)setOwned({declaration,core});})
      .catch(()=>{if(current)setOwned({declaration,error:'canvas_core_unavailable'});});
    return()=>{current=false;};
  },[declaration,runtime]);
  const current=owned&&owned.declaration===declaration?owned:null;
  const core=current?.core;
  const key=core?JSON.stringify(core.bindings.map(name=>vars&&Object.hasOwn(vars,name)?['value',vars[name]]:['missing'])):'';
  const visible=declaration&&(declaration.page===undefined||declaration.page===4||declaration.page===page);
  const projected=useMemo(()=>{try{return {spans:core&&visible?core.project(vars):[]};}catch(error){return {spans:[],error:error.code==='canvas_snapshot_unavailable'?'canvas_snapshot_unavailable':'canvas_snapshot_invalid'};}},[core,key,visible]);
  if(!declaration)return null;
  const error=current?.error||projected.error;
  return <g data-canvas-state={error?'error':core?'ready':'loading'} data-canvas-error={error||undefined} pointerEvents="none">
    {projected.spans.map(([x,y,w,h,color],i)=><rect key={i} x={x} y={y} width={w} height={h} fill={rgb565(color)}/>)}</g>;
}
