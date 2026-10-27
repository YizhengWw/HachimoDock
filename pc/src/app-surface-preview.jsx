// Runs the actual package declarations; never calls services or writes real user saves.
import React,{useEffect,useState} from 'react';
import {createRoot} from 'react-dom/client';
import {AppSurface,ComponentAppPreview} from './component-center/AppSurface.jsx';
import {initialApp,appAction,appAdvance} from './component-center/app-runtime.js';
import {MediaSurface} from './component-center/MediaSurface.jsx';
function Card({item}){
  if(item.runtime.media&&item.runtime.surface)return <article data-id={item.manifest.id}><h2>{item.manifest.name}</h2><div className="screen"><MediaSurface surface={item.runtime.surface} images={item.images}/></div><p>媒体空态预览，不发起真实播放。触屏动作另做回归测试。</p></article>;
  if(!item.runtime.surface)return <article data-id={item.manifest.id}><h2>{item.manifest.name}</h2><p>使用固件原生渲染；本页不伪造画面，以共享 C 核心回归和设备验收为准。</p></article>;
  if(item.runtime.surface?.game_view||item.runtime.surface?.scene_view||item.runtime.surface?.data_view)return <article data-id={item.manifest.id}><h2>{item.manifest.name}</h2><div className="screen"><ComponentAppPreview component={{...item.manifest,appRuntime:item.runtime,sceneSprites:item.images,defaultBindings:item.buttons}} active interactive/></div><p>{item.manifest.description}</p><small>真实资源包 · 游戏使用共享固件 C 核心；数据组件在未连接宿主时显示空态。</small></article>;
  return <ToolCard item={item}/>;
}
function ToolCard({item}){
  const [state,setState]=useState(()=>initialApp(item.runtime));
  useEffect(()=>{const timer=setInterval(()=>setState(s=>appAdvance(item.runtime,s,100)),100);return()=>clearInterval(timer);},[item]);
  return <article data-id={item.manifest.id}><h2>{item.manifest.name}</h2><div className="screen"><AppSurface runtime={item.runtime} snapshot={state} images={item.images} bindings={item.buttons} active onAction={a=>setState(s=>appAction(item.runtime,s,a))}/></div>
    <div className="buttons">{item.buttons.map(b=><button key={b.action} onClick={()=>setState(s=>appAction(item.runtime,s,b.action))}>{b.control} · {b.label}</button>)}<button onClick={()=>setState(s=>{for(let i=0;i<60;i++)s=appAdvance(item.runtime,s,1000);return s;})}>测试 +1 分钟</button><button onClick={()=>setState(initialApp(item.runtime))}>重置模拟</button></div>
    <p>{item.manifest.description}</p><small>运行状态：{state.state} · 当前页：{state.page||'首页'} · 本页面不写设备存档。</small></article>;
}
function Preview(){const [items,setItems]=useState([]),[error,setError]=useState('');useEffect(()=>{fetch('/__app_fixtures').then(r=>r.json()).then(v=>Array.isArray(v)?setItems(v):setError(v.error)).catch(()=>setError('测试包未加载'));},[]);return <><style>{`*{box-sizing:border-box}body{margin:0;background:#ebe9e3;color:#32362f;font-family:"PingFang SC",sans-serif}main{max-width:1340px;margin:40px auto;padding:10px}h1{font-size:32px;margin-bottom:12px}.intro{color:#696d63;margin-bottom:35px}.grid{display:grid;grid-template-columns:640px 640px;gap:30px}article h2{font-size:20px}.screen{width:640px;height:480px;overflow:hidden;border-radius:14px;box-shadow:0 12px 30px #30352d1a}.buttons{display:flex;flex-wrap:wrap;gap:8px;margin-top:18px}button{padding:9px 13px;background:#fff;border:1px solid #d6d9cf;border-radius:8px;cursor:pointer}p{line-height:1.8}small{color:#70756a}@media(max-width:1350px){.grid{grid-template-columns:640px}main{max-width:680px}}`}</style><main><h1>哈基米专属 · PetUI 资源包验收</h1><p className="intro">以下画面直接读取资源包并运行统一解释器，不是概念截图；尚不代表真实设备和 PC 服务已验收。</p>{error||<div className="grid">{items.map(item=><Card key={item.manifest.id} item={item}/>)}</div>}</main></>}
createRoot(document.getElementById('root')).render(<Preview/>);
