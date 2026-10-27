import React,{useEffect,useState,useRef} from 'react';
import {invoke} from '@tauri-apps/api/core';
import {useDialogFocus} from './use-dialog-focus.js';
export default function RemovedComponents({onClose,onRestored}) {
  const [items,setItems]=useState([]),[loading,setLoading]=useState(true),[error,setError]=useState(''),[busy,setBusy]=useState('');
  const acting=useRef(false),ref=useDialogFocus(onClose,Boolean(busy));
  async function load(){setLoading(true);try{setItems(await invoke('list_removed_components'));setError('');}catch(e){setError(String(e));}finally{setLoading(false);}}
  useEffect(()=>{load();},[]);
  async function restore(item){if(acting.current)return;acting.current=true;setBusy(item.recoveryId);try{await invoke('restore_removed_component',{recoveryId:item.recoveryId});await onRestored();await load();}catch(e){setError(String(e));}finally{acting.current=false;setBusy('');}}
  return <div className="modal-backdrop"><section ref={ref} role="dialog" aria-modal="true" aria-label="组件回收站" className="modal-card component-trash-dialog"><div className="modal-header"><h3>组件回收站</h3><button className="btn-secondary btn-sm" disabled={Boolean(busy)} onClick={onClose}>关闭</button></div><div className="modal-body"><p>保留移除时的全部本地资源版本与按键设置。恢复不会自动安装到设备，不会改动游戏进度或其他本机记录。客户端内置副本仍可使用。</p>{error&&<p role="alert">{error}<button className="btn-secondary btn-sm" disabled={Boolean(busy)} onClick={load}>重试</button></p>}{loading?<p role="status">正在读取…</p>:!items.length?<p>回收站为空</p>:items.map(item=><article key={item.recoveryId}><strong>{item.name}</strong><p>{new Date(item.deletedAtMs).toLocaleString()}</p><button className="btn-secondary btn-sm" disabled={Boolean(busy)} onClick={()=>restore(item)}>{busy===item.recoveryId?'恢复中…':'恢复到我的组件'}</button></article>)}</div></section></div>;
}
