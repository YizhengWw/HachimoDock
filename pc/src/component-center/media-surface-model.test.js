import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {EMPTY_MEDIA,mediaBindings,mediaQueue,rgb565,mediaReadiness} from "./media-surface-model.js";
test("readiness defaults to enabled and distinguishes manual disable, USB and firmware",()=>{
  assert.match(mediaReadiness().title,/USB/);
  assert.match(mediaReadiness({},false).message,/手动停用/);
  assert.match(mediaReadiness({connected:false},true).title,/USB/);
  assert.match(mediaReadiness({connected:true,supported:false},true).title,/固件/);
  assert.equal(mediaReadiness({connected:true,supported:true},true).title,"尚未加载电台");
  assert.match(mediaReadiness({connected:true,supported:true,queue:[{id:"a"}]},true).title,/设备选台/);
});
test("generic surface uses real media state and honest empty bindings",()=>{
  assert.equal(mediaBindings().title,"选一台，听一会");assert.equal(mediaBindings().count,"00");
  const s={...EMPTY_MEDIA,stations:[{id:"a"},{id:"b"}],current:{id:"b",name:"Live B",city:"City"},volume:15,state:"playing"};
  assert.equal(mediaBindings(s).index,"02");assert.equal(mediaBindings(s).volume,"15%");assert.equal(mediaBindings(s).title,"Live B");
  assert.equal(rgb565(0),"rgb(0,0,0)");assert.equal(rgb565(65535),"rgb(255,255,255)");
});
test("device catalog is distinct from favorites and paginated preview uses its queue",()=>{
  const s={...EMPTY_MEDIA,queue:[{id:"a"},{id:"b"}],current:{id:"b",name:"B"}};
  assert.equal(mediaBindings(s).count,"02");assert.equal(mediaBindings(s).index,"02");
  assert.equal(s.stations.length,0);assert.equal(mediaQueue(s).length,2);
  assert.equal(mediaBindings().status,"等待选台");
  assert.match(mediaReadiness({connected:true,discovering:true},true).title,/加载/);
  const ui=readFileSync(new URL('./MediaSurface.jsx',import.meta.url),'utf8');
  assert.match(ui,/Math.floor\(selected\/rows\)\*rows/);
});
test("package owns its adapter; UI uses generic host IPC",()=>{
  const p=JSON.parse(readFileSync(new URL('../../tests/fixtures/media-sample/pc/extension.json',import.meta.url)));
  assert.equal(p.engine,"pc-declarative-media-v1");
  const ui=readFileSync(new URL('./ComponentMediaPanel.jsx',import.meta.url),'utf8');
  assert(!ui.includes('radio_request'));assert(ui.includes('component_extension_request'));
});
test("surface preview matches the device's 16px base font and keeps persistent page instructions",()=>{
  const ui=readFileSync(new URL('./MediaSurface.jsx',import.meta.url),'utf8');
  assert(ui.includes('fontSize={16*(n.scale||1)}'));
  assert(ui.includes('y={n.y+15*(n.scale||1)}'));
  assert(!ui.includes('fontSize={26'));
  const {surface}=JSON.parse(readFileSync(new URL('../../tests/fixtures/media-sample/runtime/widget.json',import.meta.url)));
  for(const page of [0,1]){
    const footer=surface.elements.find(n=>n.page===page&&n.type==='text'&&n.y===444);
    assert.match(footer?.text || '', /全局(?:键)?退出/);assert(!footer.bind);
  }
  const status=surface.elements.find(n=>n.bind==='message');
  assert.equal(status.page,2);assert(status.y+status.h<=444);
});
test("media worker inherits macOS audio scheduling protection and bounded buffering",()=>{
  const code=readFileSync(new URL('../../src-tauri/src/component_media.rs',import.meta.url),'utf8');
  assert.match(code,/AudioActivity::begin_on_player_thread/);
  assert.match(code,/sync_channel::<Result<Vec<u8>, String>>\(32\)/);
  assert.match(code,/pending.len\(\) < 16/);
});
