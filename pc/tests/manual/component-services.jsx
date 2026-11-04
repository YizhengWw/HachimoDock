// Test-only, isolated transport. No microphone, audio, network, IPC or user storage.
import React from 'react';
import {createRoot} from 'react-dom/client';
import ComponentAppPanel from '../../src/component-center/ComponentAppPanel.jsx';
import ComponentMediaPanel from '../../src/component-center/ComponentMediaPanel.jsx';
import extension from '../../builtin-clawpkgs/city-radio/pc/extension.json';
import mediaRuntime from '../../builtin-clawpkgs/city-radio/runtime/widget.json';
import {EMPTY_MEDIA} from '../../src/component-center/media-surface-model.js';
import locationProfile from '../../component-profiles/weather.json';
const states=new Map(),calls=[];
let weatherCity={city:'上海',lat:31.230,lon:121.474},autoCity=false;
const control={calls,holdMediaStatus:false,pending:null};
window.__componentServiceTest=control;
async function request(command,{componentId,input}){
  calls.push({command,componentId,operation:input.operation,action:input.action,city:input.city,confirm:input.confirm,context:input.context});
  if(componentId==='test-application'&&input.operation==='status'&&control.appStatusFailure)throw Error('模拟状态读取失败');
  if(componentId==='test-weather'&&input.operation==='configure'){weatherCity=input.city;autoCity=false;}
  if(componentId==='test-weather'&&input.operation==='locate')autoCity=true;
  if(['enable','disable'].includes(input.operation))states.set(componentId,input.operation==='enable');
  const enabled=states.get(componentId)!==false;
  if(command==='component_extension_request'){
    const result={...EMPTY_MEDIA,enabled,connected:true,supported:true,queue:[{id:'test',name:'模拟广播'}]};
    if(input.operation==='status'&&control.holdMediaStatus){control.holdMediaStatus=false;await new Promise(resolve=>{control.pending=resolve;});control.pending=null;}
    return result;
  }
  if(componentId==='test-notes')return {enabled:true,service:'notes',permissions:{todos:true},values:{headline:'隔离笔记'},notes:[],ui:{mode:5,actions:control.notesDisabled?[]:['host.todo'],context:'a'.repeat(64)}};
  return {enabled,service:componentId==='test-weather'?'weather':'knowledge',permissions:{storage:true,deviceAudio:true,speechProvider:true},state:{selected:0,favorites:[],city:weatherCity,auto_city:autoCity,geo:{error:control.weatherFailure||""}},content:[],values:{title:'回归组件',headline:input.action?'操作已执行':'等待操作',metricValue:''}};
}
const app={id:'test-application',name:'回归组件',pcApplication:{service:'knowledge',actions:{'host.speak':'speak'}},appRuntime:{host:true,states:['idle'],initial_state:'idle',vars:{},tick:[],transitions:[{from:'*',on:'host.speak'}],dashboard:{title:'回归组件'},surface:{engine:'p4-app-surface-v1',background:65535,elements:[{type:'text',x:20,y:20,w:400,h:64,color:0,bind:'headline'}]}}};
const weather={...app,id:'test-weather',name:'天气回归',pcApplication:{service:'weather',location:locationProfile,actions:{'host.refresh':'refresh'}}};
const notes={...app,id:'test-notes',name:'确认回归',pcApplication:{service:'notes',actions:{'host.todo':'todo'}},appRuntime:{...app.appRuntime,transitions:[{from:'*',on:'host.todo'}],input:{engine:'p4-input-v1',confirm:{'host.todo':'将这条笔记转为待办？'}}}};
createRoot(document.getElementById('root')).render(<main style={{maxWidth:1000,margin:'30px auto'}}><h1>组件服务回归（全部模拟）</h1><ComponentAppPanel component={app} request={request}/><ComponentMediaPanel component={{id:'test-media',pcExtension:extension,mediaSurface:mediaRuntime.surface}} request={request}/><ComponentAppPanel component={weather} request={request}/><ComponentAppPanel component={notes} request={request}/></main>);
