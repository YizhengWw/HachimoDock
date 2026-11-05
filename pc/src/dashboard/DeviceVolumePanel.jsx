import GlobalAudioCuesPanel from '../GlobalAudioCuesPanel.jsx';
import React,{useEffect,useMemo,useState} from 'react';
import {invoke} from '@tauri-apps/api/core';
import {RefreshCw,Volume2,VolumeX} from 'lucide-react';
import Button from '../shell/Button';
import {createDeviceVolumeController,EMPTY_VOLUME_STATE} from './device-volume-controller.js';
import './device-volume.css';
import {useExitProtection} from '../lib/exit-protection.js';

export default function DeviceVolumePanel({usb,active=true}) {
  const [state,setState]=useState({...EMPTY_VOLUME_STATE});
  const control=useMemo(()=>createDeviceVolumeController({invoke,onState:setState}),[]);
  const engine=usb?.capabilities?.audioMasterVolume;
  useEffect(()=>{void control.setTarget({connected:usb?.connected,boardDeviceId:usb?.boardDeviceId,bootId:usb?.bootId,connectionId:usb?.connectionId,
    portName:usb?.portName,runtime:usb?.runtime,engine});},[control,usb?.connectionId,usb?.connected,usb?.boardDeviceId,usb?.bootId,usb?.portName,usb?.runtime,engine]);
  useEffect(()=>()=>control.dispose(),[control]);
  useEffect(()=>{if(!active)return;const timer=setInterval(()=>{const s=control.getSnapshot();if(s.known&&s.draft===s.value&&!s.persisted&&!s.busy)void control.refresh();},5000);return()=>clearInterval(timer);},[active,control]);
  useExitProtection(state.busy ? "设备音量请求尚未完成" : "");
  const disabled=!state.available||!state.known||state.busy;
  return <><div className="device-volume">
    <div className="device-volume__level">
      {state.known&&state.value===0?<VolumeX size={22}/>:<Volume2 size={22}/>}
      <input type="range" min="0" max="100" step="1" value={state.draft??0} disabled={disabled}
        aria-label="设备总音量" aria-valuetext={state.known?`${state.draft}%`:'尚未读取设备音量'}
        onChange={event=>control.setDraft(Number(event.target.value))}
        onPointerUp={()=>{void control.apply();}} onKeyUp={()=>{void control.apply();}}/>
      <output aria-live="polite">{state.known?`${state.draft}%`:'—'}</output>
    </div>
    <div className="device-volume__presets">
      {[0,25,50,75,100].map(value=><Button key={value} variant="ghost" size="small" disabled={disabled}
        onClick={()=>{void control.apply(value);}}>{value===0?'静音':`${value}%`}</Button>)}
      <Button variant="ghost" size="small" disabled={!state.available||state.busy}
        onClick={()=>{void control.refresh();}}><RefreshCw size={14}/>读取设备音量</Button>
    </div>
    <p className="device-volume__message" role={state.error?'alert':undefined}>
      {state.error || (!state.available ? state.reason : state.loading ? '正在读取设备音量…' : state.busy ? '正在应用，请等待设备确认…'
        : !state.known ? '请先读取设备音量' : !state.persisted ? (state.savePending===false ? '当前音量已应用，但尚未保存到设备；重启后可能恢复原值。' : '已应用，等待设备空闲后保存。') : state.value===0 ? '设备已静音。' : '音量已保存到设备。')}
    </p>
  </div><GlobalAudioCuesPanel boardDeviceId={usb?.connected?usb?.boardDeviceId:undefined}/></>;
}
