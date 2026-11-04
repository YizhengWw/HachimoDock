import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createSideCore,validateSideRuntime} from './side-core.js';
const bytes=readFileSync(new URL('./side-core.wasm',import.meta.url));
function declaration(){
 const vars=Object.fromEntries(['score','coins','lives','checkpoint'].map(v=>[v,{type:'int',init:0}]));
 return {engine:'p4-bounded-runtime-v4',initial_state:'ready',tick:[],vars,states:['ready','playing','paused','lost','won'],
  input:{engine:'p4-input-v1',hold:'jump'},
  transitions:[{from:'ready',on:'jump',to:'playing'},{from:'playing',on:'left'},{from:'playing',on:'right'},{from:'playing',on:'pause',to:'paused'},{from:'paused',on:'pause',to:'playing'}],
  surface:{engine:'p4-app-surface-v1',graphics:'p4-app-graphics-v1',images:[{id:'atlas',frame_width:32,frame_height:16}]},
  scene:{engine:'p4-side-scene-v1',width:40,height:8,rows:[...Array(6).fill('0'.repeat(40)),...Array(2).fill('1'.repeat(40))],tiles:[{flags:0},{flags:1}],
   actors:[{id:'hero',behavior:'controlled',contact:'none',x:32,y:80,width:12,height:16,skin:0}],
   physics:{run_speed:96,acceleration:800,friction:1000,gravity:800,jump_speed:280,terminal_speed:480,lives:3},
   actions:{left:'left',right:'right',jump:'jump',pause:'pause'},active_state:'playing',result_state:'lost',win_state:'won',score_var:'score',coins_var:'coins',lives_var:'lives',checkpoint_var:'checkpoint',
   art:{image:'atlas',columns:2,rows:1,scale:2,x:0,y:48,w:640,h:416,sky:27615,tiles:[0,1],skins:[{idle:[0],walk:[1],air:[0],hurt:[1],fps:8}]}}};
}
test('side core packaged adapter and WASM exactly match preview',()=>{
 assert.equal(readFileSync(new URL('./side-core.js',import.meta.url),'utf8'),readFileSync(new URL('../../skills/petui/scripts/side-core.mjs',import.meta.url),'utf8'));
 assert.deepEqual(bytes,readFileSync(new URL('../../skills/petui/scripts/side-core.wasm',import.meta.url)));
});
for(const [name,mutate] of [
 ['fraction',r=>r.scene.width=40.5],['boolean',r=>r.scene.physics.lives=true],['overflow',r=>r.scene.actors[0].x=65536],
 ['unknown field',r=>r.scene.actors[0].script='run()'],['unknown behavior',r=>r.scene.actors[0].behavior='script'],
 ['unterminated identifier',r=>r.scene.actors[0].id='hero\n'],['NUL identifier',r=>r.scene.actors[0].id='hero\0x'],
 ['boolean flags',r=>r.scene.tiles[1].flags=true],['empty frames',r=>r.scene.art.skins[0].idle=[]],
 ['null optional speed',r=>r.scene.actors[0].speed=null],['null optional reward',r=>r.scene.tiles[1].reward=null],['null optional width',r=>r.scene.art.skins[0].width=null],
 ['fraction frame',r=>r.scene.art.skins[0].idle=[0.2]],['string boolean',r=>r.scene.actors[0].solid='false'],
 ['row length',r=>r.scene.rows[0]='0'],['spawn collision',r=>r.scene.actors[0].y=96],
 ['aliased output',r=>r.scene.score_var='checkpoint'],['invalid image',r=>r.surface.images[0].frame_width=16],
 ['repeat reward',r=>Object.assign(r.scene.tiles[1],{reward:100,replacement:1})],
 ['incompatible host',r=>r.host={}],['initial running',r=>r.initial_state='playing']])test(`side core rejects ${name} before WASM coercion`,async()=>{
 const r=declaration();mutate(r);assert.throws(()=>validateSideRuntime(r));await assert.rejects(createSideCore(bytes,r));
});
test('continuous controls, short release, pause clock, frames and camera use firmware core',async()=>{
 const r=declaration(),g=await createSideCore(bytes,r);assert.equal(g.snapshot().phase,0);
 g.jump(true,0);g.jump(false,1);for(let t=20;t<=1000;t+=20)g.tick(t);
 assert(g.snapshot().actors[0].grounded);g.axis(1);for(let t=1020;t<=3000;t+=20)g.tick(t);
 let s=g.snapshot();assert(s.actors[0].x>180);assert(s.commands.length>0&&s.commands.length<=704);
 g.pause(true,3000);const before=g.snapshot();g.tick(100000);assert.deepEqual(g.snapshot().actors,before.actors);
 g.pause(false,100000);g.axis(-1);g.tick(100020);assert.equal(g.snapshot().phase,1);
 g.jump(true,100020);g.tick(100040);g.jump(false,100040);assert(g.snapshot().actors[0].vy>=-90);
});
