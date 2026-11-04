// Synthetic typed IPC responses only. No actual Tauri IPC/device/services.
import React,{useEffect,useState} from 'react';
import {createRoot} from 'react-dom/client';
import {ProgramBackgroundProvider,ProgramBackgroundStatus,ProgramBackgroundStart} from '../../src/component-center/ProgramBackground.jsx';
import '../../src/styles.css';
const model=window.__backgroundUi={ready:false,calls:[],boot:'0123456789abcdef',registration:'18446744073709551615',owned:false,live:false,revision:1,nextUnknown:false,delayNextQuery:0};
const snapshot=()=>({bootId:model.boot,items:model.owned?[{widgetId:'focus',registration:model.registration,live:model.live,stopped:!model.live,checkpointPending:true,revision:model.revision,phase:1,lastStepUs:50,maxStepUs:150}]:[]});
const request=async(name,args)=>{
  model.calls.push({name,args:structuredClone(args)});if(model.calls.length>1000)throw Error('bounded_fixture');
  if(name==='usb_get_background_programs'){
    const value=snapshot(),delay=model.delayNextQuery||20;model.delayNextQuery=0;
    await new Promise(resolve=>setTimeout(resolve,delay));return value;
  }
  await new Promise(resolve=>setTimeout(resolve,20));
  if(name==='usb_start_background_program'){
    model.owned=true;model.live=true;
    if(model.nextUnknown){model.nextUnknown=false;return {confirmed:false,code:'result_unknown_no_replay'};}
    return {confirmed:true,code:'running',snapshot:snapshot()};
  }
  if(name==='usb_stop_background_program'){
    if(args.bootId!==model.boot||args.registration!==model.registration)throw Error('stale_fixture_ticket');
    model.live=false;return {confirmed:true,code:'stopped',snapshot:snapshot()};
  }
  throw Error('unexpected_fixture_command');
};
function App(){
  const [active,setActive]=useState(true),[compatible,setCompatible]=useState(true),[epoch,setEpoch]=useState(0);
  useEffect(()=>{model.setActive=setActive;model.setCompatible=setCompatible;model.bumpBoot=()=>{model.boot='1123456789abcdef';model.revision=99;setEpoch(n=>n+1);};model.ready=true;},[]);
  const usb={connected:true,runtime:'esp-p4',boardDeviceId:'p4-ui-test',capabilities:{debugBootId:model.boot,widgetProgram:'p4-component-program-v1',widgetProgramBackground:'p4-program-background-v2',widgetProgramInspect:compatible?'p4-program-inspect-v1':null,widgetProgramBackgroundMaxActive:1}};
  return <main data-epoch={epoch} style={{maxWidth:800,margin:'32px auto',padding:24}}>
    <h1 style={{fontSize:20,marginBottom:20}}>后台程序 · 静默离线交互验证</h1>
    <ProgramBackgroundProvider usb={usb} active={active} request={request}>
      <ProgramBackgroundStatus/>
      <ProgramBackgroundStart component={{id:'focus',appRuntime:{program:{engine:'p4-component-program-v1'}}}} isInstalled installing={false}/>
    </ProgramBackgroundProvider>
  </main>;
}
createRoot(document.getElementById('root')).render(<App/>);
