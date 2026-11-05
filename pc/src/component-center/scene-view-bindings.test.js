import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import React from 'react';
import {renderToStaticMarkup} from 'react-dom/server';
import {build} from 'esbuild';
import {validateSceneViewBindings} from './graphics-scene-view.js';
import {validateClawpkgManifest} from '../lib/clawpkg-contract.js';
const fixture=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/graphics-v2.json',import.meta.url)));
test('actual scene preview renders all eight atlas bindings, including entities beyond the v1 ceiling',async()=>{
  const result=await build({entryPoints:[fileURLToPath(new URL('./SceneSurfaceLayer.jsx',import.meta.url))],bundle:true,write:false,platform:'node',format:'cjs',external:['react'],logLevel:'silent'});
  const module={exports:{}};new Function('require','module','exports',result.outputFiles[0].text)(createRequire(import.meta.url),module,module.exports);
  const surface=fixture.sceneViewBindings.valid8;
  const frame={width:16,height:12,entities:fixture.runtime.scene.entities.map(e=>({...e,width:1,height:1,tone:1,active:true}))};
  const markup=renderToStaticMarkup(React.createElement('svg',null,React.createElement(module.exports.default,{view:surface.scene_view,frame,images:[{id:'atlas',dataUrl:'data:image/png;base64,A'}],clock:100})));
  assert.equal((markup.match(/data-entity="/g)||[]).length,8);
  assert.equal((markup.match(/<image /g)||[]).length,8);
  for(const id of ['e','f','g','h'])assert.ok(markup.includes(`data-entity="${id}"`));
  assert.equal(surface.images.length,1,'eight entity bindings share a single decoded atlas');
});

test('frontend binding validation uses the same 8/4 profile ceiling',()=>{
  const v=fixture.sceneViewBindings;
  assert.doesNotThrow(()=>validateSceneViewBindings(v.valid8,fixture.runtime));
  assert.throws(()=>validateSceneViewBindings(v.valid8,fixture.runtimeV1));
  assert.throws(()=>validateSceneViewBindings(v.invalid9,fixture.runtime));
  assert.doesNotThrow(()=>validateSceneViewBindings(v.valid4,fixture.runtimeV1));
  const base={'component.json':{id:'eight-atlas',name:'Eight',version:'1'},'negative-screen.json':{},'runtime/':{},'assets/':{},'share.json':{},'buttons.json':[{event:'button.sw1.short_press',action:'tap',label:'Tap',control:'SW1'}]};
  const r=structuredClone(fixture.runtime);delete r.scene.engine;r.surface=v.valid8;
  assert.equal(validateClawpkgManifest({...base,'runtime/widget.json':r}).valid,true);
  delete r.graphics;assert.equal(validateClawpkgManifest({...base,'runtime/widget.json':r}).valid,false);
});

test('native scene source sheets keep the independent graphics-v2 8/16384 boundary',()=>{
  const r=structuredClone(fixture.runtime);delete r.scene.engine;
  r.scene.sprites=Array.from({length:8},(_,i)=>({id:`sprite${i}`,asset:`assets/sprite${i}.png`,frame_width:64,frame_height:32,frames:1,fps:1}));
  r.scene.entities.forEach((e,i)=>e.sprite=`sprite${i}`);
  const base={'component.json':{id:'eight-sheets',name:'Eight',version:'1',kind:'game'},'negative-screen.json':{},'runtime/':{},'assets/':{},'share.json':{},'buttons.json':[{event:'button.sw1.short_press',action:'tap',label:'Tap',control:'SW1'}]};
  const validate=()=>validateClawpkgManifest({...base,'runtime/widget.json':r});
  assert.equal(validate().valid,true);
  r.scene.sprites[0].frame_height=33;assert.equal(validate().valid,false,'decoded 16448 pixels exceeds 16384');r.scene.sprites[0].frame_height=32;
  delete r.graphics;assert.equal(validate().valid,false,'v1 retains four sheets and 4096 pixels');
});
