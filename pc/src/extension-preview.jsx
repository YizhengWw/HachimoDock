// Development-only mock transport. Never imported by the production application.
import React from "react";
import {createRoot} from "react-dom/client";
import ComponentMediaPanel from "./component-center/ComponentMediaPanel.jsx";
import extension from "../builtin-clawpkgs/city-radio/pc/extension.json";
import runtime from "../builtin-clawpkgs/city-radio/runtime/widget.json";
import {EMPTY_MEDIA} from "./component-center/media-surface-model.js";
import {MediaSurface} from "./component-center/MediaSurface.jsx";
const station={id:"demo",name:"示例城市广播（模拟）",city:"测试城市",codec:"AAC",hls:true};
let snapshot={...EMPTY_MEDIA,stations:[],history:[],queue:[station]},enabled=true;
async function mockRequest(_, {input}){
  switch(input.operation){
    case "permissions":return {enabled,permissions:extension.permissions};
    case "enable":enabled=true;snapshot={...snapshot,queue:[station],message:"左右切台，打开列表选择电台"};break;
    case "disable":enabled=false;return {enabled};
    case "search":return {stations:[station]};
    case "add":snapshot={...snapshot,stations:[station]};break;
    case "remove":snapshot={...snapshot,stations:[]};break;
    case "play":snapshot={...snapshot,state:"playing",current:station,history:[station]};break;
    case "stop":snapshot={...snapshot,state:"idle",message:"已停止收听"};break;
    case "toggle":snapshot={...snapshot,state:snapshot.state==="playing"?"idle":"playing",current:station};break;
    case "volume":snapshot={...snapshot,volume:input.volume};break;
    case "sleep":snapshot={...snapshot,sleepMinutes:input.minutes};break;
  }
  return {...snapshot,enabled,connected:true,supported:true};
}
const demoQueue=["北京音乐广播","上海新闻广播","广州交通广播","杭州之声","成都音乐广播","深圳新闻广播"].map((name,i)=>({id:String(i),name,city:"网络直播"}));
const design={...EMPTY_MEDIA,queue:demoQueue,current:demoQueue[0],state:"playing",message:"左右切台，打开列表选择电台",selected:1};
const images=[{id:"vinyl",dataUrl:new URL("../builtin-clawpkgs/city-radio/assets/vinyl.png",import.meta.url).href},{id:"background",dataUrl:new URL("../builtin-clawpkgs/city-radio/assets/background.png",import.meta.url).href}];
function DesignPreview(){
 const [state,setState]=React.useState("playing");
 return <><div style={{display:"flex",gap:12,marginBottom:18}}><button onClick={()=>setState("playing")}>模拟播放</button><button onClick={()=>setState("idle")}>模拟停止</button><button onClick={()=>setState("buffering")}>模拟缓冲</button></div><div style={{display:"grid",gridTemplateColumns:"1fr 1fr",gap:24}}><MediaSurface images={images} surface={runtime.surface} snapshot={{...design,state}}/><MediaSurface images={images} surface={runtime.surface} snapshot={{...design,state,page:1}}/></div></>;
}
createRoot(document.getElementById("root")).render(<main style={{maxWidth:1320,margin:"30px auto",padding:28,background:"white",borderRadius:20,fontFamily:"system-ui"}}><p style={{color:"#a76716",fontSize:13}}>视觉与交互验收 · 以下电台为模拟展示，不联网、不播放、不写入真实授权或收藏</p><DesignPreview/><ComponentMediaPanel component={{id:"city-radio",pcExtension:extension,mediaSurface:runtime.surface,sceneSprites:images}} request={mockRequest}/></main>);
