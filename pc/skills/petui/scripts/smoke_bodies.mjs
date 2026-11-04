import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createBodiesPreview} from './bodies-core.mjs';
import {actionEnabled} from './input-policy.mjs';
import {initialApp,appAdvance,appAction} from './app-runtime.mjs';
const r=JSON.parse(fs.readFileSync(process.argv[2])),buttons=JSON.parse(fs.readFileSync(process.argv[3]));
const bytes=fs.readFileSync(new URL('./classic-core.wasm',import.meta.url));
const actions=[...new Set(buttons.map(b=>b.action))],checks=[];
let state=initialApp(r),now=1,moved=false;
const core=await createBodiesPreview(bytes,r.scene,r.logic?r.vars:undefined,r.sfx?.cues?.map(c=>c.id));
function capture(){
 const s=core.snapshot();
 for(const e of s.entities){assert(Number.isFinite(e.x)&&Number.isFinite(e.y));assert(e.x>=0&&e.y>=0&&e.x+e.width<=s.width&&e.y+e.height<=s.height);}
 return s;
}
function advance(){
 for(let i=0;i<40;i++){
  now+=r.scene.tick_ms;state=appAdvance(r,state,r.scene.tick_ms);
  if(r.logic)core.setVars(state.vars);
  if(state.state===r.scene.active_state)core.tick(now);else core.hold(now);
  const frame=capture();
  if(r.logic)state.vars={...state.vars,...core.readVars()};
  if(r.scene.score_var)state.vars[r.scene.score_var]=frame.score;
  if(frame.over&&r.scene.result_state)state.state=r.scene.result_state;
 }
}
core.tick(now);capture();
for(const action of actions){
 assert(r.transitions.some(t=>t.on===action),'body action lacks a transition');
 if(!actionEnabled(r,state,action)||!r.transitions.some(t=>t.on===action&&(t.from==='*'||t.from===state.state))){checks.push({action,accepted:false});continue;}
 const before=JSON.stringify(capture()),old=JSON.stringify(state);
 const next=appAction(r,state,action);
 if(r.logic)core.setVars(next.vars);
 core.action(action,now);state=next;
 advance();const after=JSON.stringify(capture());moved||=before!==after;
 checks.push({action,changed:before!==after||old!==JSON.stringify(state)});
}
advance();
console.log(JSON.stringify({ok:true,mode:'same-firmware-bodies-core',actions:checks,moved,deviceVerified:false,errors:[]}));
