// Synthetic fixture: actual exported entrypoint and actual device C/WASM; no IPC.
import React from 'react';
import {createRoot} from 'react-dom/client';
import {flushSync} from 'react-dom';
import {ComponentAppPreview} from '../../src/component-center/AppSurface.jsx';
const runtime={schema_version:1,engine:'p4-bounded-runtime-v4',states:['idle','paused'],initial_state:'idle',vars:{},tick:[],
  input:{engine:'p4-input-v1',confirm:{restart:'Reset bodies?'}},
  transitions:[{from:'*',on:'restart',to:'idle'},{from:'*',on:'kick'},{from:'idle',on:'pause',to:'paused'},{from:'paused',on:'pause',to:'idle'}],
  scene:{tick_ms:20,active_state:'idle',auto_start:true,grid:{width:16,height:12},gravity:0,
    entities:[{id:'a',x:2,y:2,vx:4,shape:'triangle'},{id:'b',x:12,y:2}],
    bodies:[{entity:'a',rolling:true},{entity:'b',static:true}],
    rules:[{on:'restart',do:[{op:'restart'}]},{on:'kick',do:[{op:'impulse',entity:'a',dx:2,dy:0}]}]},
  surface:{engine:'p4-app-surface-v1',graphics:'p4-app-graphics-v1',background:0,
    scene_view:{engine:'p4-scene-surface-v1',x:0,y:0,w:640,h:360,colors:[65535,12345,23456,34567,45678],sprites:[]},
    elements:[{type:'button',x:20,y:400,w:180,h:56,color:12345,text:'Reset',action:'restart'},
      {type:'button',x:230,y:400,w:180,h:56,color:12345,text:'Pause',action:'pause'},
      {type:'button',x:440,y:400,w:180,h:56,color:12345,text:'Kick',action:'kick'}]}};
let component={id:'bodies-entrypoint-test',appRuntime:runtime,sceneSprites:[],defaultBindings:[]};
const root=createRoot(document.getElementById('root'));let active=true,mounted=true;
function render(){flushSync(()=>root.render(mounted?<ComponentAppPreview component={component} active={active} interactive/>:null));}
window.__bodiesEntry={
  active(value){active=value;render();},
  mount(){mounted=true;render();},
  unmount(){mounted=false;render();},
  replace(){component={...component,appRuntime:structuredClone(runtime)};render();},
  snapshot(){return [...document.querySelectorAll('[data-content-view="scene"] [data-entity]')].map(e=>({id:e.dataset.entity,x:Number(e.dataset.x),y:Number(e.dataset.y)}));},
};
render();
