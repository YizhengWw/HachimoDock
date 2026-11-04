import test from 'node:test';
import assert from 'node:assert/strict';
import {localInitial,localAction,localAdvance,localView} from './local-tools.js';
for(const count of [16,17,24,25,31,32,33,44,45,64])test(`local masks retain every saved bit for ${count} items`,()=>{
 const runtime={local:{kind:'deck',groups:Array.from({length:3},(_,g)=>({title:'group',items:Array.from({length:count},(_,i)=>({title:`item-${g}-${i}`,note:`item-${g}-${i}`}))})),actions:{'local.draw':'draw','local.exclude':'exclude','local.reset':'reset'},reveal_ms:300,idle:'idle',busy:'busy',hint:'hint'}};
 let s=localInitial({clock:0,vars:Object.fromEntries(Array.from({length:count>24?13:7},(_,i)=>[`local${i}`,i===2||i===4||i===6?-1:0]))},0);
 for(let g=0;g<3;g++){
  s.vars.local0=g;s.local.phase=0;s=localAction(runtime,s,'local.draw',0);
  const seen=new Set();
  for(let i=0;i<count;i++){
   assert.equal(s.local.phase,1);s.clock+=300;s=localAdvance(runtime,s);assert.equal(s.local.phase,2);
   const at=s.vars[`local${2+2*g}`];assert(!seen.has(at));seen.add(at);assert.equal(localView(runtime,s).note,`item-${g}-${at}`);
   s=localAction(runtime,s,'local.exclude',0);
  }
  assert.equal(s.local.phase,4);assert.equal(localView(runtime,s).metricValue,'0');
  // Exact JSON integer round-trip keeps every high bit. Production save reset
  // maps state to ready, so re-enter from phase0 before drawing the exhausted pool.
  s=JSON.parse(JSON.stringify(s));s.local.phase=0;s=localAction(runtime,s,'local.draw',0);assert.equal(s.local.phase,4);
  s=localAction(runtime,s,'local.reset',0);assert.equal(localView(runtime,s).metricValue,String(count));
 }
 console.log(`JS_BOUNDARY_OK ${count}`);
});
