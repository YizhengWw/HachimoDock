// Development-only UI fixture: all native calls are mocked. Never connects to devices or the community.
import React,{useState} from 'react';
import {createRoot} from 'react-dom/client';
import ComponentCenter from '../src/ComponentCenter.jsx';
import {DeviceContext} from '../src/shell/DeviceContext.jsx';
import ToastStack,{ToastProvider} from '../src/shell/ToastStack.jsx';
import '../src/styles.css';

const demo={id:'review-demo',name:'回归测试组件',version:'1.0.0',versionHash:'abcd',path:'/mock/library/review-demo/abcd',valid:true,kind:'tool',author:'测试',dashboard:{title:'测试预览',subtitle:'不会连接真实设备',footer:'SW1 刷新'},buttons:[{id:'refresh',action:'refresh',label:'刷新',event:'button.sw1.short_press',control:'SW1 短按'}]};
const second={...demo,id:'review-second',name:'第二个测试组件',path:'/mock/library/review-second/abcd'};
const custom={...demo,id:'snake-turn',name:'自定义蛇',version:'0.0.1',path:'/mock/library/snake-turn/abcd'};
let library=[demo,second,custom],trash=[],connected=true,failCommunity=false;
let inventory=[{id:demo.id,name:demo.name,version:'1.0.0',manifestState:'ok'},{id:'review-device-only',name:'仅设备测试组件',version:'1.0.0',manifestState:'ok'}];
const calls=[];
window.__reviewCalls=calls;
const commentRecords=[];
function record(command,args){calls.push({command,args});document.getElementById('review-log')?.replaceChildren(JSON.stringify(calls.slice(-30),null,2));}
window.__TAURI_EVENT_PLUGIN_INTERNALS__={unregisterListener:()=>{}};
window.__TAURI_INTERNALS__={transformCallback:()=>1,unregisterCallback:()=>{},convertFileSrc:x=>x,invoke:async(command,args={})=>{
  record(command,args);
  switch(command){
    case 'plugin:event|listen':return 1;
    case 'plugin:event|unlisten':return;
    case 'list_component_library':return {libraryPath:'',components:library};
    case 'list_device_widgets':return {freshness:'live',runtime:'esp32p4',transport:'usb',supportsMultiple:true,maxInstalled:50,items:inventory};
    case 'usb_get_status':return {connected,boardDeviceId:'review-board',runtime:'esp32p4'};
    case 'usb_get_button_config':return {bindings:[{action:'page_back',event:'button.sw3.short_press'}]};
    case 'install_clawpkg_over_usb':return {ok:true,manifest:{name:demo.name},errors:[]};
    case 'remove_widget_from_device':inventory=inventory.filter(x=>x.id!==args.input.componentId&&x.id!==args.input.widgetId);return {ok:true};
    case 'delete_component_from_library':trash=[{componentId:demo.id,name:demo.name,recoveryId:'fixture-backup',deletedAtMs:Date.now()}];library=library.filter(x=>x.id!==demo.id);return {deletedId:demo.id,recoveryId:'fixture-backup'};
    case 'list_removed_components':return trash;
    case 'restore_removed_component':library=[demo,...library];trash=[];return {};
    case 'community_request':{
      const a=args.input.action;
      if(a==='local')return {owned:{},downloaded:{},reservedIds:[]};
      if(a==='browse'){if(failCommunity)throw Error('模拟网络断开');return {items:[{id:'review-community',name:'社区回归样例',version:'1.0.0',summary:'隔离环境的组件',author:'测试',covers:[]}],total:1};}
      if(a==='rankings')return {items:[]};
      if(a==='mine')return {items:[]};
      if(a==='detail')return {id:'review-community',name:'社区回归样例',version:'1.0.0',summary:'隔离环境的组件',author:'测试',covers:[],available:true,updatedAt:1792600000,description:'测试说明',releaseNotes:'测试更新',license:'测试授权',commentCount:commentRecords.length,likes:0,downloads:0};
      if(a==='comments')return {items:[...commentRecords],total:commentRecords.length};
      if(a==='download')return {entry:{...demo,id:'review-community',name:'社区回归样例'}};
      if(a==='like')return {ok:true};
      if(a==='comment'){
        const input=args.input;
        const record=commentRecords.find(c=>c.requestId===input.requestId)||{id:crypto.randomUUID(),requestId:input.requestId,name:input.name,text:input.text,at:1792600000};
        if(!commentRecords.includes(record))commentRecords.push(record);
        if(window.__failCommentOnce){window.__failCommentOnce=false;throw Error('模拟回执中断');}
        return record;
      }
      if(a==='prepare'){const item=library.find(x=>x.path===args.input.path);return {...item,contentHash:'abcd',packageBytes:1000,expandedBytes:2000,files:['component.json','LICENSE.txt'],canPublish:true,hasLicense:true};}
      throw Error(`未实现的测试社区操作：${a}`);
    }
    default:throw Error(`禁止真实调用：${command}`);
  }
}};
localStorage.setItem('pet-manager.onboarding.component-center.v2','1');
// Simulate the old saved community tab; the current center must still start locally.
localStorage.setItem('pet-manager:center:tab','"discover"');
function Harness(){
  const [online,setOnline]=useState(true);
  const [failed,setFailed]=useState(false);
  return <ToastProvider><div style={{padding:16,background:'#fff1e6',position:'sticky',top:0,zIndex:2}}><strong>隔离回归：不连接硬件，不读写用户资源</strong> <button onClick={()=>{connected=!online;setOnline(!online);}}>切换模拟连接（{online?'已连接':'离线'}）</button> <button onClick={()=>{failCommunity=!failed;setFailed(!failed);}}>社区网络（{failed?'失败':'正常'}）</button><details><summary>测试调用记录</summary><pre id="review-log" style={{maxHeight:180,overflow:'auto'}}/></details></div><DeviceContext.Provider value={{usb:{connected:online,boardDeviceId:'review-board',runtime:'esp32p4',capabilities:{}},currentComponent:null}}><ComponentCenter/></DeviceContext.Provider><ToastStack/></ToastProvider>;
}
createRoot(document.getElementById('root')).render(<Harness/>);
