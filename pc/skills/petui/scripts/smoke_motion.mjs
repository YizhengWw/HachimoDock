import fs from 'node:fs';import assert from 'node:assert/strict';
import {createSceneCore} from './scene-core.mjs';import {initialApp,appAction} from './app-runtime.mjs';
const r=JSON.parse(fs.readFileSync(process.argv[2])),buttons=JSON.parse(fs.readFileSync(process.argv[3]));
const bytes=fs.readFileSync(new URL('./classic-core.wasm',import.meta.url));
const action=r.input.hold,pause=r.lifecycle.pause_action,step=r.scene.tick_ms;
assert(buttons.some(b=>b.action===action&&b.event==='button.sw1.short_press'));
assert(buttons.some(b=>b.action===pause));
const create=()=>createSceneCore(bytes,r.scene);
let scored=false,failed=false,scoredCore;
for(let hold=0;hold<=r.scene.motion.charge_ms;hold+=40){
 const g=await create();let s=appAction(r,initialApp(r),action);assert.equal(s.state,r.scene.active_state);
 g.action(action,100);let t=100;for(;t<100+hold;t+=step)g.tick(t+step);
 const before=g.snapshot();g.release(false,t);let moved=false;
 for(let i=0;i<200;i++){t+=step;g.tick(t);if(JSON.stringify(g.snapshot().entities)!==JSON.stringify(before.entities))moved=true;}
 assert(moved,'release must move');
 if(g.snapshot().score>0){scored=true;scoredCore=g;}
 if(g.snapshot().over){failed=true;g.action(action,t+step);assert(!g.snapshot().over&&g.snapshot().running&&g.snapshot().score===0);}
}
assert(scored,'no input duration can score');assert(failed,'no reachable losing round');
const g=await create();g.action(action,100);g.tick(240);assert(g.snapshot().charge>0);
let s=appAction(r,initialApp(r),action);s=appAction(r,s,pause);assert.notEqual(s.state,r.scene.active_state);
g.release(true,240);const before=g.snapshot();g.hold(10000);assert.deepEqual(g.snapshot(),before);
s=appAction(r,s,pause);assert.equal(s.state,r.scene.active_state);g.tick(10020);assert.deepEqual(g.snapshot().entities,before.entities);
const visibleActions=[action,pause];
for(const b of buttons.filter(b=>!visibleActions.includes(b.action))){
 assert(r.scene.rules.some(rule=>rule.on===b.action&&rule.do.some(op=>op.op==='restart')),'untested motion action: '+b.action);
 assert(r.input.confirm?.[b.action],'manual restart must ask before discarding the round');
 for(const state of r.states){
  const old=initialApp(r);old.state=state;old.vars[r.lifecycle.record_var]=7;
  const next=appAction(r,old,b.action);assert.equal(next.state,r.scene.active_state);assert.equal(next.vars[r.lifecycle.record_var],7);
 }
 scoredCore.action(b.action,20000);assert.equal(scoredCore.snapshot().score,0);assert(scoredCore.snapshot().running&&!scoredCore.snapshot().over);assert.equal(scoredCore.snapshot().charge,0);
 visibleActions.push(b.action);
}
console.log(JSON.stringify({ok:true,mode:'same-firmware-motion-core',visibleActions,started:true,charged:true,released:true,score:true,result:true,restart:true,cancel:true,pause:true,deviceVerified:false,errors:[]}));
