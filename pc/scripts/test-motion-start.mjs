import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createSceneCore} from '../src/component-center/scene-core.js';
const runtime=JSON.parse(fs.readFileSync(new URL('../new-components/cloud-hop/runtime/widget.json',import.meta.url)));
const wasm=fs.readFileSync(new URL('../src/component-center/classic-core.wasm',import.meta.url));
const g=await createSceneCore(wasm,runtime.scene);
const ready=g.snapshot();
g.tick(60000);assert.deepEqual(g.snapshot().entities,ready.entities);
g.action(runtime.input.hold,60000);
assert.deepEqual(g.snapshot().entities,ready.entities,'first charge must preserve the board shown on entry');
g.tick(60020);assert(g.snapshot().charge<5,'idle time must not count towards charge');
g.release(true,60020);
g.action(runtime.input.hold,61000);
assert.deepEqual(g.snapshot().entities,ready.entities,'cancel and retry must not reroll the track');
g.tick(61020);g.release(false,61020);
assert.deepEqual(g.snapshot().entities,ready.entities,'release only launches, never regenerates platforms');
// Underpowered repeated hops eventually fall; only then should charging reset.
let now=61020;
for(let attempt=0;attempt<30&&!g.snapshot().over;attempt++){
  for(let i=0;i<100;i++){now+=20;g.tick(now);}
  if(!g.snapshot().over){g.action(runtime.input.hold,now);g.release(false,now);}
}
assert(g.snapshot().over);
g.action(runtime.input.hold,now+20);
const restarted=g.snapshot();assert(!restarted.over);assert(restarted.running);assert.equal(restarted.travel,0);assert.equal(restarted.score,0);
assert.notDeepEqual(restarted.entities,ready.entities,'a completed round may get a new track');
console.log('PASS entry → delayed start → cancel → retry → release preserves platforms; game-over restart regenerates.');
