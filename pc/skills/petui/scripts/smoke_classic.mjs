import fs from 'node:fs/promises';
import assert from 'node:assert/strict';
import {createClassicCore} from './classic-core.mjs';
const runtime=JSON.parse(await fs.readFile(process.argv[2],'utf8')),bindings=JSON.parse(await fs.readFile(process.argv[3],'utf8'));
const bytes=await fs.readFile(new URL('./classic-core.wasm',import.meta.url)),config=runtime.game;
const primary=config.actions.start||config.actions.flap,visible=[];
for(const b of bindings){
  const g=await createClassicCore(bytes,runtime),control=await createClassicCore(bytes,runtime);
  assert(Object.values(config.actions).includes(b.action)||b.action===runtime.lifecycle?.pause_action,`Unknown gameplay action: ${b.action}`);
  if(b.action===primary){
    const before=g.snapshot();g.action(b.action,1);
    assert.notDeepEqual(g.snapshot(),before,`No start feedback: ${b.action}`);
  }else{
    g.action(primary,1);control.action(primary,1);
    if(config.type==='snake'&&config.direction_mode==='absolute'&&[config.actions.left,config.actions.right].includes(b.action)){
      // Start facing east. West is correctly rejected and east is already current.
      // First establish a northbound step so either horizontal turn is observable.
      g.action(config.actions.up,2);control.action(config.actions.up,2);
      g.tick(config.tick_ms+2);control.tick(config.tick_ms+2);
    }
    const offset=config.type==='snake'&&config.direction_mode==='absolute'&&[config.actions.left,config.actions.right].includes(b.action)?config.tick_ms+3:2;
    g.action(b.action,offset);
    for(const t of [offset+config.tick_ms,offset+config.tick_ms*2]){g.tick(t);control.tick(t);}
    assert.notDeepEqual(g.snapshot(),control.snapshot(),`Action is no-op (automatic ticks excluded): ${b.action}`);
  }
  visible.push(b.action);
}
const g=await createClassicCore(bytes,runtime);g.action(primary,1);assert(g.snapshot().running);
let t=1;
for(;t<200000&&!g.snapshot().over;t+=config.tick_ms){if(config.type==='blocks')g.action(config.actions.drop,t);g.tick(t);}
assert(g.snapshot().over,'result unreachable');
g.action(primary,t+1);assert(g.snapshot().running&&!g.snapshot().over,'restart failed');
console.log(JSON.stringify({ok:true,mode:'shared-firmware-core',visibleActions:visible,started:true,ticksAdvanced:true,result:true,restart:true,deviceVerified:false,errors:[]}));
