import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createSideCore} from './side-core.mjs';
const r=JSON.parse(fs.readFileSync(process.argv[2])),buttons=JSON.parse(fs.readFileSync(process.argv[3]));
const bytes=fs.readFileSync(new URL('./side-core.wasm',import.meta.url)),s=r.scene;
for(const [key,event] of Object.entries({left:'knob.rotate_ccw',right:'knob.rotate_cw',jump:'button.sw1.short_press',pause:'button.sw2.short_press'}))assert(buttons.some(b=>b.event===event&&b.action===s.actions[key]));
const create=()=>createSideCore(bytes,r),test=await create();
assert.equal(test.snapshot(false).phase,0);test.action(s.actions.jump,0);test.tick(20);assert.equal(test.snapshot(false).phase,1);
for(let t=40;t<=1000;t+=20)test.tick(t);
for(const key of ['left','right']){
 const before=test.snapshot(false),x=before.actors[before.player].x;
 test.action(s.actions[key],1000);for(let t=1020;t<=1100;t+=20)test.tick(t);test.axis(0);
 assert.notEqual(test.snapshot(false).actors[before.player].x,x,`${key} must move through actual bound input`);
}
test.action(s.actions.pause,1100);const frozen=test.snapshot(false);test.tick(9000);assert.deepEqual(test.snapshot(false).actors,frozen.actors);assert.equal(frozen.phase,2);
test.action(s.actions.pause,9000);assert.equal(test.snapshot(false).phase,1);
const initial=s.rows.map(row=>[...row].map(v=>parseInt(v,16)));
const solid=(x,y)=>x<0||x>=s.width?true:y<0||y>=s.height?false:!!(s.tiles[initial[y][x]].flags&3);
const goal=s.actors.find(v=>v.contact==='goal'),player=s.actors.find(v=>v.behavior==='controlled');
const dir=goal&&goal.x<player.x?-1:1;let finished=null,attempts=0;
// Bounded input-only search; never writes actor coordinates or simulation memory.
for(const distance of [12,20,28,36,44]){
 for(const duration of [220,300,380,460]){
  const core=await create();let now=0,release=300,jumps=1;core.jump(true,now);core.axis(dir);let before=core.snapshot(false);
  for(let frame=0;frame<3000;frame++){
   now+=20;if(release&&now>=release){core.jump(false,now);release=0;}
   core.axis(dir);const p=before.actors[before.player],body=s.actors[before.player];
   const tx=Math.floor((p.x+(dir>0?body.width:0)+dir*distance)/16),foot=Math.floor((p.y+body.height)/16);
   const wall=[Math.floor(p.y/16),Math.floor((p.y+body.height-1)/16)].some(y=>solid(tx,y));
   const gap=!solid(tx,foot)&&!solid(tx,foot+1);
   const enemy=before.actors.some((v,i)=>v.active&&s.actors[i].contact==='damage'&&(v.x-p.x)*dir>-4&&(v.x-p.x)*dir<distance+22&&Math.abs(v.y-p.y)<24);
   if(p.grounded&&!release&&(wall||gap||enemy)){core.jump(true,now);release=now+duration;jumps++;}
   core.tick(now);before=core.snapshot(false);if(before.phase===3||before.phase===4)break;
  }
  attempts++;if(before.phase===(goal?3:4)&&before.score>0){finished={core,now,before,jumps};break;}
 }
 if(finished)break;
}
assert(finished,'bounded input-only play could not reach a scoring outcome; this is not proof of impossibility');
const {core,now,before,jumps}=finished;core.jump(false,now);core.jump(true,now+20);
assert.equal(core.snapshot(false).phase,1);assert.equal(core.snapshot(false).score,0);assert.equal(core.snapshot(false).lives,s.physics.lives);
console.log(JSON.stringify({ok:true,mode:'same-firmware-side-core',started:true,visibleActions:Object.values(s.actions),tickAdvanced:true,score:before.score,coins:before.coins,lives:before.lives,checkpoint:before.checkpoint,result:before.phase===3?'won':'lost',restart:true,pause:true,jumps,virtualMs:now,attempts,maxAttempts:20,maxStepsPerAttempt:3000,deviceVerified:false,errors:[]}));
