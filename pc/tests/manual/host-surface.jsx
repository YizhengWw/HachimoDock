// Test-only: accepts exported synthetic host snapshots; no IPC or real services.
import React from 'react';
import {createRoot} from 'react-dom/client';
import {AppSurface} from '../../src/component-center/AppSurface.jsx';
import {initialApp} from '../../src/component-center/app-runtime.js';
const root=createRoot(document.getElementById('root'));
window.__hostSurface={calls:[],show(item,fixture){
  this.item ||= item;item=this.item; // Stable package identity, as in the real panel.
  window.__hostSurface.calls=[];
  root.render(<><style>{'body{margin:0;background:#eae9e5}.screen{width:640px;height:480px}'}</style>
    <div className="screen"><AppSurface runtime={item.runtime} images={item.images} bindings={item.buttons}
      snapshot={{...initialApp(item.runtime),values:fixture.values,ui:fixture.ui}}
      onAction={(action,confirmation)=>window.__hostSurface.calls.push({action,...confirmation})}/></div></>);
}};
