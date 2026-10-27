import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createSceneCore} from '../src/component-center/scene-core.js';
const runtime=JSON.parse(fs.readFileSync(new URL('../new-components/cloud-hop/runtime/widget.json',import.meta.url)));
// Retain legacy v1 coverage independently of the resource's new track mode.
delete runtime.scene.motion.track;
runtime.scene.motion.speed=[2,13];runtime.scene.motion.platforms=['home','next'];runtime.scene.entities=runtime.scene.entities.slice(0,3);
runtime.scene.rules=runtime.scene.rules.filter(r=>r.on!=='landed'||r.with==='next');
runtime.scene.rules.find(r=>r.on==='landed').do=[{op:'score',add:1},{op:'place',entity:'puppy',x:2,y:4},{op:'place',entity:'next',x:[7,12],y:6}];
const wasm=fs.readFileSync(new URL('../src/component-center/classic-core.wasm',import.meta.url));
const fresh=()=>createSceneCore(wasm,runtime.scene);
let g=await fresh();g.action('jump.charge',100);for(let t=120;t<=800;t+=20)g.tick(t);
assert(g.snapshot().charge>=57&&g.snapshot().charge<=60);
g.release(false,800);g.tick(820);assert(g.snapshot().entities[0].y<4);assert.equal(g.snapshot().score,0);
g.action('jump.charge',830);assert.equal(g.snapshot().charge,0,'airborne hold ignored');
for(let t=840;t<=1800;t+=20)g.tick(t);
assert.equal(g.snapshot().score,1);assert.equal(g.snapshot().entities[0].y,4);assert(g.snapshot().entities[2].x>=7);
const before=g.snapshot();g.action('jump.charge',1900);g.tick(2100);g.release(true,2100);g.hold(10000);g.tick(10020);
assert.deepEqual(g.snapshot().entities,before.entities);assert.equal(g.snapshot().charge,0);
// Max charge stays capped, full jump falls past the initial target. Restart resets score.
g=await fresh();g.action('jump.charge',100);for(let t=120;t<=3100;t+=20)g.tick(t);assert.equal(g.snapshot().charge,100);
g.release(false,3100);for(let t=3120;t<=5000;t+=20)g.tick(t);assert(g.snapshot().over);
g.action('jump.charge',5100);assert.equal(g.snapshot().score,0);assert(!g.snapshot().over);
g.release(true,5100);assert.equal(g.snapshot().charge,0);
const corrupt=structuredClone(runtime.scene);corrupt.motion.body='missing';await assert.rejects(createSceneCore(wasm,corrupt));
const unsupported=structuredClone(runtime.scene);unsupported.entities[0].y=3;await assert.rejects(createSceneCore(wasm,unsupported));
console.log('PASS motion: charge, release, ascent, landing/score, randomized next platform, airborne rejection, cancel, pause clock, cap, falling, restart, invalid body/support.');
