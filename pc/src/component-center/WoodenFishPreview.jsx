import React, { useEffect, useRef, useState } from "react";
import { loadLayer, motion, paint, WoodenFishAudio } from "./wooden-fish.js";

export default function WoodenFishPreview({ active = true }) {
  const canvas=useRef(null),hit=useRef(-Infinity),audio=useRef(null);
  const trajectory=useRef({y:0,scale:1});
  const invalidate=useRef(()=>{});
  const [error,setError]=useState("");
  const [musicPlaying,setMusicPlaying]=useState(false);
  const stop=()=>{audio.current?.close();audio.current=null;setMusicPlaying(false);};
  useEffect(()=>{
    if(!active)stop();
    const controller=new AbortController();let frame,body,mallet,live=true;
    const draw=now=>{
      frame=null;
      if(!live)return;
      const age=now-hit.current;
      if(canvas.current)paint(canvas.current.getContext("2d"),body,mallet,age,trajectory.current.y,trajectory.current.scale);
      if(active&&age<900)frame=requestAnimationFrame(draw);
    };
    invalidate.current=()=>{if(frame==null)frame=requestAnimationFrame(draw);};
    Promise.all(["body","mallet"].map(n=>loadLayer(n,controller.signal)))
      .then(layers=>{if(live){[body,mallet]=layers;invalidate.current();}}).catch(e=>{if(live&&e.name!=="AbortError")setError(e.message);});
    invalidate.current();
    const hidden=()=>{if(document.hidden)stop();};
    document.addEventListener("visibilitychange",hidden);window.addEventListener("blur",stop);
    return()=>{live=false;controller.abort();cancelAnimationFrame(frame);stop();document.removeEventListener("visibilitychange",hidden);window.removeEventListener("blur",stop);};
  },[active]);
  const strike=event=>{
    event.stopPropagation();if(!active)return;
    const now=performance.now();
    const current=motion(now-hit.current,trajectory.current.y,trajectory.current.scale);
    trajectory.current={y:current.malletY,scale:current.fishScale};
    hit.current=now;audio.current ||= new WoodenFishAudio();
    invalidate.current();
    const player=audio.current;
    player.strike().then(playing=>{if(audio.current===player){setMusicPlaying(playing);setError("");}})
      .catch(e=>{if(audio.current===player){setError(e.message);stop();}});
  };
  return <div style={{width:"100%",height:"100%",position:"relative",background:"#f1e3cf"}}>
    <canvas ref={canvas} width="640" height="480" role="button" tabIndex={active?0:-1}
      aria-label="敲木鱼：点击或按空格敲一下，首次敲击后开启轻音乐"
      onClick={strike} onKeyDown={event=>{if([" ","Enter"].includes(event.key)){event.preventDefault();if(!event.repeat)strike(event);}if(event.key==="Escape")stop();}}
      style={{display:"block",width:"100%",height:"100%",objectFit:"contain",cursor:"pointer",borderRadius:16}} />
    <div role="status" style={{position:"absolute",top:"6.5%",right:"5%",fontSize:11,color:"#9a8871",pointerEvents:"none"}}>
      {musicPlaying?"轻音乐已开启":"点击木鱼开启轻音乐"}
    </div>
    {error&&<div role="alert" style={{position:"absolute",bottom:30,left:20,right:20,color:"#a23e28",background:"#fff8",padding:8}}>{error}</div>}
  </div>;
}
