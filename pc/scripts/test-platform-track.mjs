import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createSceneCore} from '../src/component-center/scene-core.js';
const runtime=JSON.parse(fs.readFileSync(new URL('../new-components/cloud-hop/runtime/widget.json',import.meta.url)));
const wasm=fs.readFileSync(new URL('../src/component-center/classic-core.wasm',import.meta.url));
const scene=runtime.scene,m=scene.motion,step=scene.tick_ms;
const fresh=(s=scene)=>createSceneCore(wasm,s);
let g=await fresh(),now=100,worldX=-1;
const colors=new Set(),widths=new Set(),gaps=new Set();
function frame(){
  const s=g.snapshot(),body=s.entities.find(e=>e.id===m.body);
  assert.equal(s.entities.length,5,'fixed pool, not accumulating objects');
  const x=body.x+s.travel;
  assert(x>=worldX-0.001,'must not teleport back to start');
  if(worldX>=0)assert(x-worldX<0.5,'no forward teleport either');
  worldX=x;
  for(const e of s.entities){assert(e.x>-12&&e.x<90,'rebased, bounded coordinates');}
  return s;
}
// Match integer integration to choose a reachable hold, rather than a lucky seed.
let y=0,vy=-m.lift*256,flight=0;
do{vy+=Math.trunc(m.gravity*256*step/1000);y+=Math.trunc(vy*step/1000);flight++;}while(y<0);
for(let jump=0;jump<1000;jump++){
  g.action(runtime.input.hold,now);
  let s=g.snapshot(),b=s.entities[0];
  const platforms=s.entities.filter(e=>m.platforms.includes(e.id)).sort((a,b)=>a.x-b.x);
  const current=platforms.find(p=>p.x<=b.x+b.width/2&&b.x+b.width/2<p.x+p.width);
  assert(current,'every new jump starts on actual landing platform');
  const next=platforms.find(p=>p.x>=current.x+current.width);
  assert(next,'generated path must always extend ahead');
  const distance=next.x+next.width/2-(b.x+b.width/2);
  let hold=0,error=Infinity;
  for(let ms=0;ms<=m.charge_ms;ms+=step){
    const vx=256*m.speed[0]+Math.trunc(256*(m.speed[1]-m.speed[0])*ms/m.charge_ms);
    const d=flight*Math.trunc(vx*step/1000)/256;
    if(Math.abs(d-distance)<error){error=Math.abs(d-distance);hold=ms;}
  }
  assert(error<next.width/2-.15,'generated target reachable with safe margin');
  colors.add(next.tone);widths.add(next.width);gaps.add(Math.round(next.x-current.x-current.width));
  for(let elapsed=0;elapsed<hold;elapsed+=step){now+=step;g.tick(now);frame();}
  g.release(false,now);assert(!g.snapshot().grounded);
  for(let i=0;i<100;i++){
    now+=step;g.tick(now);s=frame();assert(!s.over,`jump ${jump} fell`);
    if(s.grounded)break;
  }
  assert(s.grounded);assert.equal(s.score,jump+1);
  for(let i=0;i<32;i++){now+=step;g.tick(now);frame();}
}
assert.equal(colors.size,4);assert.equal(widths.size,3);assert(gaps.size>=3);
assert(g.snapshot().travel>5000,'camera must genuinely travel forward');
// Pause clock/cancel retain position and score; no queued jump after resume.
const before=g.snapshot();g.action(runtime.input.hold,now);now+=100;g.tick(now);g.release(true,now);g.hold(now+60000);now+=60020;g.tick(now);
assert.deepEqual(g.snapshot().entities,before.entities);assert.equal(g.snapshot().score,1000);
// Return to current platform does not score; underpowered fall can restart.
g=await fresh();now=100;g.action(runtime.input.hold,now);g.release(false,now);
for(let i=0;i<80;i++){now+=step;g.tick(now);}
assert.equal(g.snapshot().score,0);
let lost=false;
for(let i=0;i<20&&!lost;i++){
  g.action(runtime.input.hold,now);g.release(false,now);
  for(let j=0;j<100;j++){now+=step;g.tick(now);}
  lost=g.snapshot().over;
}
assert(lost,'short hops eventually miss the gap');g.action(runtime.input.hold,now+20);
assert.equal(g.snapshot().score,0);assert.equal(g.snapshot().travel,0);assert(!g.snapshot().over);
for(const change of [s=>s.motion.track.gap=[1,255],s=>s.motion.track.tones=[1],s=>s.motion.track.camera_x=0,s=>s.entities[2].active=false,s=>s.rules[1].do=[{op:'place',entity:m.body,x:2,y:4}]]){
  const bad=structuredClone(scene);change(bad);await assert.rejects(fresh(bad));
}
console.log('PASS 1000 continuous platform landings: no reset/teleport, bounded pool, 4 colors, 3 widths, varied reachable gaps, smooth camera, pause/cancel, no duplicate score, fall/restart, malformed rejection.');
