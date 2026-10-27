import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createSceneCore} from '../src/component-center/scene-core.js';
const runtime=JSON.parse(fs.readFileSync(new URL('../new-components/cloud-hop/runtime/widget.json',import.meta.url)));
const wasm=fs.readFileSync(new URL('../src/component-center/classic-core.wasm',import.meta.url));
const {scene}=runtime,m=scene.motion,step=scene.tick_ms;
let edgeLandings=0,misses=0,cases=0;
// Real package + public hold/release/tick API: cover every achievable charge
// at 20ms intervals, including delayed display updates with bounded catch-up.
for(const batch of [1,4,8])for(let hold=0;hold<=m.charge_ms;hold+=step){
  const g=await createSceneCore(wasm,scene);let now=100;
  g.action(runtime.input.hold,now);
  for(let ms=0;ms<hold;ms+=step){now+=step;g.tick(now);}
  const start=g.snapshot(),b=start.entities[0];
  const charge=Math.min(hold+1,m.charge_ms);
  const vx=256*m.speed[0]+Math.trunc(256*(m.speed[1]-m.speed[0])*charge/m.charge_ms);
  let x=0,y=0,vy=-m.lift*256,contact;
  do{
    const oldY=y,oldX=x;
    vy+=Math.trunc(m.gravity*256*step/1000);
    x+=Math.trunc(vx*step/1000);y+=Math.trunc(vy*step/1000);
    if(vy>=0&&oldY<=0&&y>=0){contact=oldX+Math.trunc((x-oldX)*(-oldY)/(y-oldY));break;}
  }while(y<0);
  assert(Number.isFinite(contact));
  const centre=b.x+b.width/2+contact/256,halfFoot=b.width/4;
  const target=start.entities.find(p=>m.platforms.includes(p.id)&&centre+halfFoot>p.x&&centre-halfFoot<p.x+p.width);
  g.release(false,now);
  for(let i=0;i<120;i++){now+=batch*step;g.tick(now);if(g.snapshot().grounded||g.snapshot().over)break;}
  const end=g.snapshot();assert.equal(end.grounded,!!target,`hold=${hold}, batch=${batch}`);
  if(target){
    assert(Math.abs(end.entities[0].x+end.travel-(b.x+contact/256))<1/256,'no post-contact horizontal overshoot');
    assert.equal(end.score,target.id==='home'?0:1);
    if(centre<target.x||centre>=target.x+target.width)edgeLandings++;
  }else{assert(end.over);assert.equal(end.score,0);misses++;}
  cases++;
}
assert(edgeLandings>0);assert(misses>0);
console.log(`PASS ${cases} real-package charge/landing cases: ${edgeLandings} foot-only edge landings, ${misses} genuine misses; normal and delayed frames.`);
