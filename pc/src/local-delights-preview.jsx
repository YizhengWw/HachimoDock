import React,{useEffect,useState} from 'react';
import {createRoot} from 'react-dom/client';
import {AppSurface} from './component-center/AppSurface.jsx';
import {initialApp,appAction,appAdvance} from './component-center/app-runtime.js';
function Device({item}){
 const [s,set]=useState(()=>initialApp(item.runtime));
 useEffect(()=>{let t=setInterval(()=>set(s=>appAdvance(item.runtime,s,100)),100);return()=>clearInterval(t)},[item]);
 return <><div style={{width:640,height:480}}><AppSurface runtime={item.runtime} snapshot={s} images={item.images} bindings={item.buttons} active onAction={a=>set(s=>appAction(item.runtime,s,a))}/></div><p>{item.manifest.name} · 当前状态 {s.state}</p><p>{item.manifest.description}</p><div>{item.buttons.map(b=><button key={b.action} onClick={()=>set(s=>appAction(item.runtime,s,b.action))}>{b.control} · {b.label}</button>)}</div><nav>{['decision-capsule','answer-book','hope-train'].map(id=><a key={id} href={`?id=${id}`}>{id} </a>)}</nav></>;
}
function App(){const[item,set]=useState(null);useEffect(()=>{fetch('/__app_fixtures').then(r=>r.json()).then(items=>set(items.find(i=>i.manifest.id===new URLSearchParams(location.search).get('id'))||items[0]));},[]);return <><style>{`body{margin:0;background:#e9e7e2;font-family:system-ui}p,nav{margin:20px;max-width:600px}button{margin:8px;padding:12px}nav a{display:inline-block;margin:12px}`}</style>{item?<Device key={item.manifest.id} item={item}/>:<p>正在加载资源包</p>}</>}
createRoot(document.getElementById('root')).render(<App/>);
