import React, { useState } from "react";
import { createRoot } from "react-dom/client";
import WoodenFishPreview from "./component-center/WoodenFishPreview.jsx";
function Preview() {
const [active,setActive]=useState(true);
return <main style={{minHeight:"100vh",boxSizing:"border-box",background:"#eee9e1",padding:"48px 24px",fontFamily:"system-ui",color:"#65533d"}}>
  <header style={{maxWidth:800,margin:"0 auto 24px"}}><span style={{fontSize:12,letterSpacing:3}}>HACHIMO · QUIET MOMENT</span><h1 style={{fontWeight:500,fontSize:25,margin:"12px 0"}}>给自己，留一刻安静。</h1><p style={{fontSize:14,color:"#948472"}}>点击木鱼或按空格敲击。首次敲击开启轻音乐；Esc 静音，切走窗口自动停止。</p></header>
  <section onPointerDown={()=>{document.querySelectorAll("audio").forEach(a=>a.pause());setActive(true);}} style={{maxWidth:800,aspectRatio:"4/3",margin:"auto",borderRadius:24,overflow:"hidden",boxShadow:"0 20px 65px #75624822",border:"1px solid #fff9"}}><WoodenFishPreview active={active} /></section>
  <section style={{maxWidth:800,margin:"28px auto",display:"grid",gap:12}}>
    <div style={{background:"#faf6ef",borderRadius:16,padding:"16px 20px",display:"flex",alignItems:"center",gap:12,flexWrap:"wrap"}}>
      <div style={{flex:"1 1 160px"}}><strong style={{fontWeight:500}}>单独听背景音乐</strong><div style={{fontSize:12,marginTop:5,color:"#948472"}}>已调高混音电平，仍低于木鱼敲击音</div></div>
      <audio controls preload="none" src="/instruments/wooden-fish/ambience.wav?v=4" aria-label="背景音乐试听" onPlay={e=>{setActive(false);document.querySelectorAll("audio").forEach(a=>{if(a!==e.currentTarget)a.pause();});}} style={{width:270,maxWidth:"100%",height:36}} />
    </div>
    <h2 style={{fontSize:18,fontWeight:500,margin:0}}>已选 A · 清脆短促</h2>
    <p style={{fontSize:13,margin:"0 0 4px",color:"#948472"}}>上方木鱼已使用 A 音效，保留其余试听供对比。每段三次敲击，不混背景音乐。</p>
    {[["A-crisp","A · 清脆短促","音头更亮，收尾干净"],["B-hollow","B · 空灵木质","中音共鸣，余韵稍长"],["C-soft","C · 厚实柔和","音调低一些，温润不刺耳"]].map(([id,title,description])=><div key={id} style={{background:"#faf6ef",borderRadius:16,padding:"16px 20px",display:"flex",alignItems:"center",gap:12,flexWrap:"wrap"}}>
      <div style={{flex:"1 1 160px"}}><strong style={{fontWeight:500}}>{title}</strong><div style={{fontSize:12,marginTop:5,color:"#948472"}}>{description}</div></div>
      <audio controls preload="none" src={`/instruments/wooden-fish/${id}.wav`} aria-label={title} onPlay={e=>{setActive(false);document.querySelectorAll("audio").forEach(a=>{if(a!==e.currentTarget)a.pause();});}} style={{width:270,maxWidth:"100%",height:36}} />
    </div>)}
  </section>
  <footer style={{maxWidth:800,margin:"22px auto",fontSize:12,color:"#948472",lineHeight:1.9}}>交互预览 · 本地合成木质敲击音与原创轻柔伴奏。<br/>设备中默认短按 SW1 敲击，全局退出和实时对话按键优先。此页面不是已刷入设备的证明。</footer>
</main>;
}
createRoot(document.getElementById("root")).render(<Preview />);
