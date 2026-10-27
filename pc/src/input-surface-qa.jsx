// Local browser regression harness. No service calls, playback or user data.
import React,{useState} from "react";
import {createRoot} from "react-dom/client";
import {MediaSurface} from "./component-center/MediaSurface.jsx";
const surface={background:0,elements:[
  {type:"list",x:20,y:20,w:500,h:144,color:65535},
  {type:"meter",x:40,y:250,w:400,h:4,color:65535,bind:"volume"},
  {type:"transport",x:280,y:340,w:70,h:70,color:65535,action:"media.toggle"}
]};
function App(){
  const [rows,setRows]=useState([{id:"a",name:"测试 A"},{id:"b",name:"测试 B"}]);
  const [calls,setCalls]=useState([]),[epoch,setEpoch]=useState(0),[width,setWidth]=useState(640);
  return <><p>仅测试输入，不播放音频。</p><button data-testid="reset" onClick={()=>{setCalls([]);setEpoch(e=>e+1);}}>重置</button>
    <button data-testid="replace" onClick={()=>setRows([{id:"c",name:"替换 C"},{id:"a",name:"测试 A"}])}>替换队列</button>
    <button data-testid="scale" onClick={()=>setWidth(320)}>缩小</button>
    <pre data-testid="calls">{JSON.stringify(calls)}</pre><div data-testid="media" style={{width}}>
      <MediaSurface key={epoch} surface={surface} snapshot={{source:"test",queue:rows,volume:35,state:"idle"}} command={c=>setCalls(v=>[...v,c])}/>
    </div></>;
}
createRoot(document.getElementById("root")).render(<App/>);
