import React, {useState} from 'react';
import {createRoot} from 'react-dom/client';
import TextModelSelect from './src/TextModelSelect.jsx';
import './src/styles.css';
const rows = [{id:'doubao-seed-2-1-lite-260915'}, {id:'doubao-seed-2-1-pro-260915'}, {id:'old',status:'Shutdown'}];
window.__modelReviewCalls=[];
window.__TAURI_INTERNALS__={invoke: async (command,args)=> {
  window.__modelReviewCalls.push({command,method:args?.method});
  if(command==='list_voice_chat_models')return rows.slice(0,2).map(x=>x.id);
  if(command==='http_request_text')return {status:200,body:JSON.stringify({data:rows})};
  throw Error('Offline review does not allow this command');
}};
function Review(){const [model,setModel]=useState('doubao-seed-2-1-lite-260915');const[vision,setVision]=useState('ep-user-saved');return <main style={{padding:32,maxWidth:900,margin:'auto'}}>
  <h1>模型选择 · 离线交互验收</h1><p>使用虚拟目录，不访问云端，不调用模型或麦克风。</p>
  <section className="card" style={{padding:24,marginBottom:20}}><h2>对话大模型</h2><TextModelSelect value={model} onChange={setModel} configured baseUrl="https://ark.cn-beijing.volces.com/api/v3"/><output id="chat-choice">{model}</output></section>
  <section className="card" style={{padding:24}}><h2>图像理解 / 提示词模型</h2><TextModelSelect vision label="图像理解模型" value={vision} onChange={setVision} apiKey="test-offline-key" baseUrl="https://ark.cn-beijing.volces.com"/><output id="vision-choice">{vision}</output></section></main>}
createRoot(document.getElementById('root')).render(<Review/>);
