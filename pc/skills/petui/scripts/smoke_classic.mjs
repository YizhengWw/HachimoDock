import fs from 'node:fs/promises';
import assert from 'node:assert/strict';
import {createClassicCore} from './classic-core.mjs';
import {initialApp,appAction,appAdvance,takeCues} from './app-runtime.mjs';
const runtime=JSON.parse(await fs.readFile(process.argv[2],'utf8')),bindings=JSON.parse(await fs.readFile(process.argv[3],'utf8'));
const bytes=await fs.readFile(new URL('./classic-core.wasm',import.meta.url)),config=runtime.game;
const primary=config.actions.start||config.actions.flap||config.actions.place,visible=[];
const gomoku=config.type==='gomoku';
const cues=new Set(),runtimeVars=new Set();
// gomoku: walk the cursor to the lowest free cell and place there; the device
// answers on the next tick. A naive scan always loses to the bounded AI.
function gomokuPlay(g,t){
  const s=g.snapshot(),w=s.width,cursor=s.cells.findIndex(c=>c&4),target=s.cells.findIndex(c=>!(c&3));
  if(cursor<0||target<0)return t;
  let cx=cursor%w,cy=Math.floor(cursor/w);const tx=target%w,ty=Math.floor(target/w);
  while(cx>tx){g.action(config.actions.left,t);cx--;}while(cx<tx){g.action(config.actions.right,t);cx++;}
  while(cy>ty){g.action(config.actions.up,t);cy--;}while(cy<ty){g.action(config.actions.down,t);cy++;}
  g.action(config.actions.place,t);g.tick(t+config.tick_ms);return t+config.tick_ms;
}
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
    if(gomoku&&b.action===config.actions.undo){
      // Undo needs a stone pair on the board first: place at the centre, let white answer.
      g.action(config.actions.place,2);control.action(config.actions.place,2);
      g.tick(config.tick_ms+2);control.tick(config.tick_ms+2);
    }
    const offset=(config.type==='snake'&&config.direction_mode==='absolute'&&[config.actions.left,config.actions.right].includes(b.action))||(gomoku&&b.action===config.actions.undo)?config.tick_ms+3:2;
    g.action(b.action,offset);
    for(const t of [offset+config.tick_ms,offset+config.tick_ms*2]){g.tick(t);control.tick(t);}
    assert.notDeepEqual(g.snapshot(),control.snapshot(),`Action is no-op (automatic ticks excluded): ${b.action}`);
  }
  visible.push(b.action);
  // Runtime side of the same binding: transition cues (e.g. place/undo sounds).
  let a=appAction(runtime,initialApp(runtime),primary);if(b.action!==primary)a=appAction(runtime,a,b.action);
  for(const c of takeCues(a))cues.add(c);
}
// Runtime tick[] and sfx keep running beside the native core on the device (the
// firmware tick loop precedes the game step). Mirror that here: every core action is
// also a runtime action, every core tick advances the runtime clock, the engine score
// is mirrored into score_var, and queued cues are collected.
let app=initialApp(runtime),appClock=0;
function syncApp(core){const g=core.snapshot();app={...app,state:g.state||app.state,vars:{...app.vars,[config.score_var]:g.score}};}
function appAct(core,action,now){syncApp(core);app=appAction(runtime,app,action);for(const c of takeCues(app))cues.add(c);}
function appTick(core,now){syncApp(core);const before=app.vars;app=appAdvance(runtime,app,Math.max(0,now-appClock));appClock=now;for(const c of takeCues(app))cues.add(c);
  for(const k of Object.keys(app.vars))if(k!==config.score_var&&app.vars[k]!==before[k])runtimeVars.add(k);}
const g=await createClassicCore(bytes,runtime);g.action(primary,1);appAct(g,primary,1);assert(g.snapshot().running);
let t=1;
if(gomoku){for(let moves=0;moves<120&&!g.snapshot().over;moves++){const s=g.snapshot(),w=s.width,cursor=s.cells.findIndex(c=>c&4),target=s.cells.findIndex(c=>!(c&3));
  if(cursor<0||target<0)break;let cx=cursor%w,cy=Math.floor(cursor/w);const tx=target%w,ty=Math.floor(target/w);t+=1;
  const step=(a)=>{g.action(a,t);appAct(g,a,t);};
  while(cx>tx){step(config.actions.left);cx--;}while(cx<tx){step(config.actions.right);cx++;}while(cy>ty){step(config.actions.up);cy--;}while(cy<ty){step(config.actions.down);cy++;}
  step(config.actions.place);for(let k=50;k<=config.tick_ms;k+=50){g.tick(t+k);appTick(g,t+k);}t+=config.tick_ms;
  // A player thinks between moves; idle once so periodic runtime ticks (rhythm loops) are observed.
  if(moves===1)for(let k=50;k<=4000;k+=50){g.tick(t+k);appTick(g,t+k);}if(moves===1)t+=4000;}}
else for(;t<200000&&!g.snapshot().over;t+=config.tick_ms){if(config.type==='blocks'){g.action(config.actions.drop,t);appAct(g,config.actions.drop,t);}g.tick(t);appTick(g,t);}
assert(g.snapshot().over,'result unreachable');
for(let k=100;k<=2000;k+=100){g.tick(t+k);appTick(g,t+k);} // result-state ticks (settlement cues) get two seconds
g.action(primary,t+2001);appAct(g,primary,t+2001);assert(g.snapshot().running&&!g.snapshot().over,'restart failed');
const declared=(runtime.sfx?.cues||[]).map(c=>c.id),silent=declared.filter(c=>!cues.has(c));
if(runtime.sfx)assert(cues.size>0,'sfx declared but no cue is reachable through play');
console.log(JSON.stringify({ok:true,mode:'shared-firmware-core',visibleActions:visible,started:true,ticksAdvanced:true,result:true,restart:true,
  runtimeTicks:{declared:(runtime.tick||[]).length,changedVars:[...runtimeVars].sort()},sfxCues:{declared,reached:[...cues].sort(),unreached:silent},deviceVerified:false,errors:[]}));
