import test from 'node:test';
import assert from 'node:assert/strict';
import {createDeviceVolumeController,MASTER_VOLUME_ENGINE} from './device-volume-controller.js';
const target={connected:true,boardDeviceId:'board-test',connectionId:'1',bootId:'boot-1',runtime:'esp-p4',engine:MASTER_VOLUME_ENGINE};
const reply=(value=50,board='board-test')=>({ok:true,engine:MASTER_VOLUME_ENGINE,boardDeviceId:board,volume:value,persisted:true});
const deferred=()=>{let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return{promise,resolve,reject};};
test('unsupported firmware makes no writes and no invented initial volume',async()=>{let calls=0;const c=createDeviceVolumeController({invoke:()=>{calls++;}});await c.setTarget({...target,engine:null});assert.equal(c.getSnapshot().value,null);assert.equal(await c.apply(50),false);assert.equal(calls,0);});
test('query before slider admission, apply once and retain exact device confirmation',async()=>{const calls=[];const c=createDeviceVolumeController({invoke:async(command,args)=>{calls.push({command,args});return reply(args.volume??50);}});await c.setTarget(target);assert.equal(calls[0].args.volume,undefined);assert.equal(c.getSnapshot().value,50);c.setDraft(25);await c.apply();assert.equal(calls[1].args.volume,25);await c.apply(25);assert.equal(calls.length,2);});
test('unknown mutation result blocks further sets until read-only reconciliation',async()=>{const calls=[];const c=createDeviceVolumeController({invoke:async(_,args)=>{calls.push(args);if(args.volume!==undefined)throw Error('lost reply');return reply();}});await c.setTarget(target);await c.apply(25);assert.equal(c.getSnapshot().known,false);await c.apply(25);assert.equal(calls.length,2);await c.refresh();assert.equal(calls.length,3);assert.equal(calls[2].volume,undefined);assert.equal(c.getSnapshot().known,true);});
test('late reply from retired USB connection cannot update new connection',async()=>{const d=deferred();let n=0;const c=createDeviceVolumeController({invoke:()=>++n===1?d.promise:Promise.resolve(reply(75))});const old=c.setTarget(target);await c.setTarget({...target,connectionId:'2'});d.resolve(reply(10));await old;assert.equal(c.getSnapshot().value,75);});
test('wrong board, fractional, missing persistence and mismatched set echoes fail closed',async()=>{for(const invalid of [reply(50,'other'),reply(1.5),{...reply(),persisted:undefined},{...reply(),ok:1}]){const c=createDeviceVolumeController({invoke:async()=>invalid});await c.setTarget(target);assert.equal(c.getSnapshot().known,false);}let n=0;const c=createDeviceVolumeController({invoke:async()=>reply(++n===1?50:75)});await c.setTarget(target);await c.apply(25);assert.equal(c.getSnapshot().known,false);});
test('busy release events do not duplicate sets, StrictMode disposal can reactivate',async()=>{const d=deferred();let calls=0;const c=createDeviceVolumeController({invoke:async(_,args)=>{calls++;return args.volume===undefined?reply():d.promise;}});await c.setTarget(target);const set=c.apply(25);await c.apply(25);assert.equal(calls,2);d.resolve(reply(25));await set;c.dispose();await c.setTarget(target);assert.equal(c.getSnapshot().known,true);assert.equal(calls,3);});

test('explicit same-value action may repair unsaved boot fallback without automatic writes',async()=>{
  const calls=[];const c=createDeviceVolumeController({invoke:async(_,args)=>{calls.push(args);return args.volume===undefined?{...reply(100),persisted:false,savePending:false}:reply(100);}});
  await c.setTarget(target);assert.equal(calls.length,1);assert.equal(c.getSnapshot().persisted,false);
  await c.apply(100);assert.equal(calls.length,2);assert.equal(calls[1].volume,100);assert.equal(c.getSnapshot().persisted,true);
  await c.apply(100);assert.equal(calls.length,2);
});
