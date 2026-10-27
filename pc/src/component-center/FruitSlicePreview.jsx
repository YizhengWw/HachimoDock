import React,{useEffect,useRef,useState} from 'react';
import coreUrl from './fruit-core.wasm?url';
import {createFruitCore} from './fruit-core.js';
import './fruit-slice.css';

export function drawFruitScene(ctx,s,images) {
  const cream='#f6f5e0',gold='#ffc65c',muted='#8db19f',deep='#102823',panel='#1d3f35',mint='#9edc95',coral='#ff7757';
  const modes=['经典挑战','限时冲分','轻松练习'];
  const rules=['漏三果或碰炸弹结束','漏果不扣分，炸弹 -30','没有炸弹，尽情划切'];
  const rect=(x,y,w,h,r,color)=>{ctx.fillStyle=color;ctx.beginPath();ctx.roundRect(x,y,w,h,r);ctx.fill();};
  const text=(value,x,y,size,color=cream,align='left',weight=600)=>{ctx.fillStyle=color;ctx.font=`${weight} ${size}px system-ui,sans-serif`;ctx.textAlign=align;ctx.fillText(value,x,y);};
  const ellipse=(x,y,rx,ry,color)=>{ctx.fillStyle=color;ctx.beginPath();ctx.ellipse(x,y,rx,ry,0,0,Math.PI*2);ctx.fill();};
  const spark=(x,y,r,color)=>{ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();ctx.moveTo(x-r,y);ctx.lineTo(x+r,y);ctx.moveTo(x,y-r);ctx.lineTo(x,y+r);ctx.stroke();};
  const fruit=(kind,x,y,angle,radius,half=0,age=0)=>{
    const img=images[kind];if(!img)return;const size=radius*2.4;
    ctx.save();ctx.translate(x,y);ctx.rotate(angle*Math.PI/180);ctx.globalAlpha=age>350?Math.max(0,(650-age)/300):1;
    if(half){ctx.beginPath();ctx.rect(half<0?-size/2:0,-size/2,size/2,size);ctx.clip();}
    ctx.drawImage(img,-size/2,-size/2,size,size);ctx.restore();
  };
  const bg=ctx.createLinearGradient(0,0,0,480);bg.addColorStop(0,'#102b26');bg.addColorStop(1,'#17442c');
  ctx.fillStyle=bg;ctx.fillRect(0,0,640,480);
  ellipse(630,135,112,220,'#1b3d30');ellipse(-40,470,240,100,'#1d412e');
  for(let i=0;i<11;i++)ellipse(23+i*61,115+(i*73)%287,1,1,'#405b40');
  if(s.phase===0||s.phase===3){
    rect(32,26,159,26,13,panel);text('FRESH FRUIT CLUB',45,44,12,mint,'left',700);
    if(s.phase===0){
      text('鲜果一刀',32,119,43,cream,'left',800);text('一指划开，快乐爆汁。',36,168,17,muted,'left',400);
      ellipse(456,249,109,15,deep);
      fruit(3,449,156,-18,47);fruit(0,537,107,24,32);fruit(1,533,220,18,27);fruit(2,354,203,-27,22);
      spark(367,91,7,gold);spark(570,176,5,mint);spark(415,245,4,coral);
      text(`当前模式最佳  ${s.best}`,36,241,15,gold);
    }else{
      text(s.bombHit?'哎呀，切到炸弹了':'漂亮！本局收刀',36,99,28);
      text(s.score,320,193,65,gold,'center',800);
      text(`切中 ${s.cutCount} 果     最高连击 ${s.maxCombo}     最佳 ${s.best}`,320,242,16,muted,'center');
    }
    text('选择你的节奏',34,288,15);
    for(let i=0;i<3;i++){
      const x=32+i*192,selected=s.mode===i;
      rect(x,306,184,72,16,deep);rect(x,302,184,72,16,selected?cream:panel);
      text(modes[i],x+16,335,19,selected?deep:cream);
      text(rules[i],x+12,359,12,selected?'#3e684d':muted,'left',400);
      if(selected)ellipse(x+164,319,4,4,coral);
    }
    rect(185,403,270,56,27,deep);rect(185,399,270,56,27,gold);
    text(s.phase===3?'再来一局':'开始切果',320,435,24,deep,'center',750);
    text('点击模式卡选择 · 开始键开局 · 全局键退出',320,474,12,muted,'center',400);
    return;
  }
  rect(24,20,128,72,18,deep);text('SCORE',40,43,12,muted);text(s.score,40,80,31);
  rect(236,24,168,34,17,panel);text(modes[s.mode],320,47,16,mint,'center');text(`第 ${s.level} 阶`,320,88,13,muted,'center');
  rect(493,20,123,72,18,deep);
  const left=Math.max(0,s.duration-Math.floor(s.elapsed/1000));
  text(`${String(Math.floor(left/60)).padStart(2,'0')}:${String(left%60).padStart(2,'0')}`,554,66,29,left<=10?coral:cream,'center');
  if(s.mode===0)for(let i=0;i<s.lives;i++)ellipse(47+i*20,112,5,5,i<s.misses?panel:coral);
  else text('尽情切，别停手',28,119,13,muted);
  for(const f of s.fruit){if(!f.active)continue;const r=f.radius;
    if(f.bomb){
      ellipse(f.x+3,f.y+5,r+2,r+2,deep);ellipse(f.x,f.y,r,r,'#2b3037');ellipse(f.x-8,f.y-9,8,10,'#55636b');
      ctx.strokeStyle=gold;ctx.lineWidth=3;ctx.beginPath();ctx.moveTo(f.x+10,f.y-r+3);ctx.lineTo(f.x+20,f.y-r-13);ctx.stroke();
      spark(f.x+20,f.y-r-13,5,coral);text('×',f.x,f.y+9,27,cream,'center');
    }else if(f.cut){const gap=f.cutAge*.055;fruit(f.kind,f.x-gap,f.y,f.angle-gap,r,-1,f.cutAge);fruit(f.kind,f.x+gap,f.y+gap*.4,f.angle+gap,r,1,f.cutAge);}
    else{
      ellipse(f.x+3,f.y+6,r,r,deep);
      if(f.golden){for(let a=0;a<360;a+=20){const rad=a*Math.PI/180;ellipse(f.x+Math.cos(rad)*(r+7),f.y+Math.sin(rad)*(r+7),2,2,gold);}spark(f.x+r+6,f.y-r-6,4,gold);}
      fruit(f.kind,f.x,f.y,f.angle,r);
    }
  }
  for(const p of s.particles)if(p.life)ellipse(p.x,p.y,p.life>250?3:2,p.life>250?5:2,[coral,gold,mint,'#ff86a1'][p.kind]);
  ctx.lineCap='round';
  for(let i=1;i<s.trail.length;i++){const a=s.trail[i-1],b=s.trail[i];if(b.start)continue;
    ctx.lineWidth=3+i/s.trail.length*4;ctx.strokeStyle=gold;ctx.beginPath();ctx.moveTo(a.x,a.y);ctx.lineTo(b.x,b.y);ctx.stroke();
    ctx.lineWidth=1.5;ctx.strokeStyle=cream;ctx.stroke();
  }
  if(s.feedbackMs){
    const x=Math.max(80,Math.min(560,s.feedbackX)),y=Math.max(135,s.feedbackY-40-(650-s.feedbackMs)/22);
    text(`${s.feedbackKind===3?'-':s.feedbackKind===2?'金果 +':'+'}${s.points}`,x,y+20,24,s.feedbackKind===3?coral:gold,'center');
  }
  if(s.comboMs&&s.combo>=3)text(`连击 ×${s.combo}`,320,142,25,gold,'center');
  rect(24,414,592,45,16,deep);text(s.feverMs?'爆汁时刻 · 双倍得分':'果汁能量',40,443,15,s.feverMs?gold:muted);
  rect(287,431,222,7,3,panel);const energy=s.feverMs?s.feverMs/50:s.energy;
  if(energy)rect(287,431,222*energy/100,7,3,s.feverMs?gold:mint);
  text(s.feverMs?`${Math.ceil(s.feverMs/1000)}s ×2`:`${s.energy}%`,530,443,14,s.feverMs?gold:muted);
  if(s.phase===1){rect(528,100,88,48,16,panel);text('暂停',572,132,16,cream,'center');}
  if(s.phase===2){
    rect(143,170,354,191,25,deep);text('休息一下，果汁不急',320,232,25,cream,'center');
    text('当前进度已暂停',320,268,16,muted,'center');rect(190,280,260,58,24,gold);text('继续切果',320,319,24,deep,'center');
  }
}

export default function FruitSlicePreview({scene,sprites=[],active=true,interactive=false}) {
  const canvas=useRef(null),engine=useRef(null),images=useRef([]),finger=useRef(null),audio=useRef(null);
  const [ready,setReady]=useState(false),[error,setError]=useState('');
  const config=JSON.stringify(scene),sources=JSON.stringify(sprites);
  const now=()=>Math.floor(performance.now());
  const draw=()=>{if(engine.current&&canvas.current)drawFruitScene(canvas.current.getContext('2d'),engine.current.snapshot(),images.current);};
  useEffect(()=>{
    let cancelled=false;setReady(false);setError('');
    Promise.all([fetch(coreUrl).then(r=>{if(!r.ok)throw Error('游戏核心加载失败');return r.arrayBuffer();}).then(bytes=>createFruitCore(bytes,JSON.parse(config))),
      Promise.all(JSON.parse(sources).map(s=>new Promise((resolve,reject)=>{const img=new Image();img.onload=()=>resolve(img);img.onerror=()=>reject(Error('水果素材加载失败'));img.src=s.dataUrl;})))])
      .then(([core,art])=>{if(!cancelled){engine.current=core;images.current=art;setReady(true);draw();}})
      .catch(e=>{if(!cancelled)setError(e.message);});
    return()=>{cancelled=true;engine.current=null;finger.current=null;};
  },[config,sources]);
  useEffect(()=>{
    if(!ready)return;
    if(!active){engine.current?.suspend(now());finger.current=null;draw();return;}
    let frame;
    const step=()=>{const e=engine.current;if(e){const t=now();e.tick(t);
      if(finger.current)e.pointer(1,finger.current.x,finger.current.y,t);
      const cue=e.sound();if(cue&&audio.current&&interactive){
        const ctx=audio.current,osc=ctx.createOscillator(),gain=ctx.createGain();osc.type=cue&2?'sawtooth':'triangle';
        osc.frequency.setValueAtTime(cue&2?95:cue&4?660:1100,ctx.currentTime);
        osc.frequency.exponentialRampToValueAtTime(cue&2?45:cue&4?880:330,ctx.currentTime+.12);
        gain.gain.setValueAtTime(0,ctx.currentTime);gain.gain.linearRampToValueAtTime((scene.sound_volume/100)*.09,ctx.currentTime+.005);gain.gain.exponentialRampToValueAtTime(.0001,ctx.currentTime+.15);
        osc.connect(gain);gain.connect(ctx.destination);osc.start();osc.stop(ctx.currentTime+.16);
      }draw();}frame=requestAnimationFrame(step);};
    frame=requestAnimationFrame(step);return()=>cancelAnimationFrame(frame);
  },[ready,active,interactive,scene.sound_volume]);
  useEffect(()=>()=>{audio.current?.close();},[]);
  const action=(type)=>{if(!engine.current)return;finger.current=null;
    if(!audio.current)audio.current=new (window.AudioContext||window.webkitAudioContext)();audio.current.resume();
    if(type==='start')engine.current.start(now(),Date.now()>>>0);else engine.current.pause(now());draw();};
  const point=e=>{const r=canvas.current.getBoundingClientRect();return {x:Math.round((e.clientX-r.left)*640/r.width),y:Math.round((e.clientY-r.top)*480/r.height)};};
  const down=e=>{if(!interactive||!engine.current)return;e.stopPropagation();
    if(!audio.current)audio.current=new (window.AudioContext||window.webkitAudioContext)();audio.current.resume();
    if(finger.current){engine.current.pointer(3,0,0,now());finger.current=null;return;}
    const p=point(e);finger.current={...p,id:e.pointerId};e.currentTarget.setPointerCapture(e.pointerId);engine.current.pointer(0,p.x,p.y,now());};
  const move=e=>{if(!finger.current||finger.current.id!==e.pointerId)return;const p=point(e);finger.current={...p,id:e.pointerId};engine.current.pointer(1,p.x,p.y,now());};
  const up=e=>{if(finger.current?.id===e.pointerId){const p=point(e);engine.current?.pointer(e.type==='pointerup'?2:3,p.x,p.y,now());finger.current=null;draw();}};
  return <div className="fruit-slice-preview" onClick={interactive?e=>e.stopPropagation():undefined}>
    <canvas ref={canvas} width="640" height="480" aria-label="鲜果一刀触屏游戏预览" style={{touchAction:interactive?'none':'auto'}} onPointerDown={down} onPointerMove={move} onPointerUp={up} onPointerCancel={up} onLostPointerCapture={up}/>
    {error&&<p role="alert">{error}</p>}
    {interactive&&<div className="fruit-slice-controls"><button type="button" disabled={!ready} onClick={()=>action('start')}>开始 / 重开</button><button type="button" disabled={!ready} onClick={()=>action('pause')}>模式 / 暂停 / 继续</button><span>点选模式 · 鼠标拖动或触屏划切</span></div>}
  </div>;
}
