import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createSceneCore} from './scene-core.js';

// p4-sfx-v1 scene side through the shared C core: rules queue cue bits, the host drains them once.
const bytes=readFileSync(new URL('./classic-core.wasm',import.meta.url));
const suite=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/sfx-v1.json',import.meta.url)));
assert.equal(suite.profile,'p4-sfx-v1');
const scene={tick_ms:300,active_state:'play',grid:{width:8,height:8},
 entities:[{id:'player',x:3,y:6},{id:'bullet',x:3,y:5,vy:-2,active:false,bounds:'hide',shape:'bullet'}],
 rules:[
  {on:'start',do:[{op:'restart'}]},
  {on:'fire',do:[{op:'place',entity:'bullet',source:'player',dy:-1},{op:'sfx',cue:'shot'}]},
  {on:'blocked',entity:'bullet',do:[{op:'hide',entity:'bullet'},{op:'sfx',cue:'thud'}]}
 ]};
test('scene rules queue declared cues and the host drains them once',async()=>{
 const c=await createSceneCore(bytes,scene,undefined,['shot','thud']);
 c.action('start',0);
 assert.deepEqual(c.takeSfx(),[]);
 c.action('fire',10);
 assert.equal(c.snapshot().entities[1].active,true,'the visible part of the rule still runs');
 assert.deepEqual(c.takeSfx(),['shot']);
 assert.deepEqual(c.takeSfx(),[],'drained once');
 c.action('fire',20);c.action('fire',30);
 assert.deepEqual(c.takeSfx(),['shot'],'a bitmask: the same cue twice in one drain plays once');
});
test('an undeclared cue is refused at load, matching the validators',async()=>{
 await assert.rejects(createSceneCore(bytes,scene,undefined,['shot']),/Unknown sfx cue/);
 await assert.rejects(createSceneCore(bytes,scene),/Unknown sfx cue/);
});
test('contract presets are the firmware preset table',()=>{
 const core=readFileSync(new URL('../../../firmware/main/pet_p4_sfx_core.c',import.meta.url),'utf8');
 const names=[...core.matchAll(/^\s*\{"([a-z]+)",/gm)].map(m=>m[1]);
 assert.deepEqual(names,suite.presets);
});
