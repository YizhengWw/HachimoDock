import React from 'react';
import {createRoot} from 'react-dom/client';
import FruitSlicePreview from './component-center/FruitSlicePreview.jsx';
import runtime from '../builtin-clawpkgs/fresh-fruit-slice/runtime/widget.json';
const files=import.meta.glob('../builtin-clawpkgs/fresh-fruit-slice/assets/*.png',{eager:true,query:'?url',import:'default'});
const sprites=runtime.scene.sprites.map(s=>({id:s.id,dataUrl:files['../builtin-clawpkgs/fresh-fruit-slice/'+s.asset]}));
createRoot(document.getElementById('root')).render(<main style={{maxWidth:850,margin:'40px auto',padding:24,fontFamily:'system-ui'}}><h1 style={{fontSize:24}}>鲜果一刀</h1><p style={{color:'#74695c'}}>与设备共用游戏核心 · 拖动划切 · 60 秒挑战</p><FruitSlicePreview scene={runtime.scene} sprites={sprites} interactive/></main>);
