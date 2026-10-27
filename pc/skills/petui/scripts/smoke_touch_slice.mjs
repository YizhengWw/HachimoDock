import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createFruitCore} from './fruit-core.mjs';
const [runtimePath,buttonsPath]=process.argv.slice(2);
const runtime=JSON.parse(fs.readFileSync(runtimePath)),buttons=JSON.parse(fs.readFileSync(buttonsPath));
assert.deepEqual(buttons.map(x=>x.action).sort(),['slice.pause','slice.restart']);
const core=await createFruitCore(fs.readFileSync(new URL('./fruit-core.wasm',import.meta.url)),runtime.scene);
assert.equal(core.snapshot().phase,0);
core.start(0,42);assert.equal(core.snapshot().phase,1);
let now=0,scored=false,moved=false;
for(let i=0;i<350;i++){
  now+=16;core.tick(now);
  const s=core.snapshot();if(s.phase!==1)break;
  const f=s.fruit.find(f=>f.active&&!f.bomb&&!f.cut&&f.y>110&&f.y<420);
  if(f){
    moved=true;core.pointer(0,Math.round(f.x-43),Math.round(f.y),now);
    core.pointer(1,Math.round(f.x+43),Math.round(f.y),now+1);
    core.pointer(2,Math.round(f.x+43),Math.round(f.y),now+2);
  }
  if(core.snapshot().score>0){scored=true;break;}
}
assert.ok(scored&&moved,'real pointer segments must cut a spawned moving fruit');
core.pause(now);const paused=core.snapshot();assert.equal(paused.phase,2);
core.tick(now+5000);assert.equal(core.snapshot().elapsed,paused.elapsed);
core.pause(now+5000);assert.equal(core.snapshot().phase,1);
core.suspend(now+5016);assert.equal(core.snapshot().phase,2);
core.start(0,23);
for(now=16;now<(runtime.scene.duration_sec+10)*1000;now+=16){core.tick(now);if(core.snapshot().phase===3)break;}
assert.equal(core.snapshot().phase,3,'miss/timeout result must be reachable');
core.start(now+1,42);assert.equal(core.snapshot().score,0);assert.equal(core.snapshot().phase,1);
const practice=await createFruitCore(fs.readFileSync(new URL('./fruit-core.wasm',import.meta.url)),runtime.scene);
practice.pointer(0,500,330,0);practice.pointer(2,500,330,1);
assert.equal(practice.snapshot().mode,2,'mode card tap must select practice');
practice.pointer(0,300,420,2);practice.pointer(2,300,420,3);
assert.equal(practice.snapshot().phase,1,'start surface must respond to a real tap');
const radii=new Set();let sawGold=false;
for(let t=19;t<(runtime.scene.duration_sec*1000)+20;t+=16){
  practice.tick(t);const s=practice.snapshot();
  for(const f of s.fruit)if(f.active){assert.equal(f.bomb,false);radii.add(f.radius);sawGold ||= f.golden;}
}
assert.equal(practice.snapshot().phase,3);assert.ok(practice.snapshot().misses>runtime.scene.lives);
assert.ok(radii.size>=4&&sawGold,'size variation and golden waves must actually spawn');
practice.pause(runtime.scene.duration_sec*1000+30);assert.equal(practice.snapshot().mode,0);
practice.pause(runtime.scene.duration_sec*1000+31);assert.equal(practice.snapshot().mode,1);
console.log(JSON.stringify({ok:true,mode:'touch-slice',sharedDeviceCore:true,deviceVerified:false,
  visibleActions:buttons.map(x=>x.action),checks:['start','moving-fruit','pointer-sweep-score','pause-freezes-time','resume','navigation-suspends','miss-result','restart','mode-card-tap','start-surface-tap','practice-no-bombs','practice-miss-safe','variable-radii','golden-wave','three-mode-selection'],errors:[]}));
