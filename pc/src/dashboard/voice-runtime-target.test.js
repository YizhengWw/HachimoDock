import test from 'node:test';
import assert from 'node:assert/strict';
import {voiceRuntimeTarget,createVoiceRuntimePoller} from './voice-runtime-target.js';
const target=(runtime,board='board')=>voiceRuntimeTarget({connected:true,runtime,boardDeviceId:board,connectionId:'1',bootId:'0011223344556677'},'agent');
test('unknown/disconnected handshake never starts a voice service',async()=>{
 for(const usb of [{},{connected:true},{connected:false,runtime:'esp-p4',boardDeviceId:'board'},{connected:true,runtime:'unknown',boardDeviceId:'board'},{connected:true,runtime:'esp-p4'}]){
  const t=voiceRuntimeTarget(usb),rows=[];
  await createVoiceRuntimePoller({target:t,invoke:()=>assert.fail('must not invoke'),publish:r=>rows.push(r),isCurrent:()=>true}).poll();
  assert.equal(t.kind,'waiting');assert.equal(rows[0].mode,'waiting_device');
 }
});
test('confirmed P4 calls only noninteractive native ensure; known legacy stays explicit',async()=>{
 for(const [runtime,method] of [['esp-p4','ensure_device_voice_runtime'],['linux','ensure_voice_runtime']]){
  const calls=[],rows=[];
  await createVoiceRuntimePoller({target:target(runtime),invoke:async(...args)=>{calls.push(args);return {running:true};},publish:r=>rows.push(r),isCurrent:()=>true}).poll();
  assert.equal(calls.length,1);assert.equal(calls[0][0],method);assert.equal(rows[0].running,true);
  if(runtime==='esp-p4')assert.deepEqual(calls[0][1],{input:{interactive:false}});
 }
});
test('old legacy success AND rejection cannot replace a newly confirmed P4 target',async()=>{
 for(const rejects of [false,true]){
  let settle;const old=target('linux'),fresh=target('esp-p4'),rows=[];let active=old.key;
  const poller=createVoiceRuntimePoller({target:old,invoke:()=>new Promise((resolve,reject)=>{settle=rejects?reject:resolve;}),publish:r=>rows.push(r),isCurrent:()=>active===old.key});
  const pending=poller.poll();active=fresh.key;
  await createVoiceRuntimePoller({target:fresh,invoke:async()=>({running:true}),publish:r=>rows.push(r),isCurrent:()=>active===fresh.key}).poll();
  settle(rejects?new Error('missing legacy node'): {running:false,message:'old'});await pending;
  assert.equal(rows.length,1);assert.equal(rows[0].targetKey,fresh.key);assert.equal(rows[0].running,true);
 }
});
test('cancelled/inflight poll does not replay or publish late results',async()=>{
 let settle,calls=0;const rows=[],t=target('esp-p4');
 const poller=createVoiceRuntimePoller({target:t,invoke:()=>{calls++;return new Promise(r=>{settle=r;});},publish:r=>rows.push(r),isCurrent:()=>true});
 const pending=poller.poll();await poller.poll();poller.cancel();settle({running:true});await pending;await poller.poll();
 assert.equal(calls,1);assert.equal(rows.length,0);
});

// Production normalization -> target -> actual poller, with no disconnected
// snapshot in between: the native connection counter is the identity change.
import {normalizeUsbStatusPayload,usbStatusSnapshotsEqual} from '../shell/DeviceContext.pure.js';
test('same-board fast reconnect rejects old success/error even with no disconnected observation',async()=>{
 for(const rejects of [false,true]){
  const raw={connected:true,runtime:'esp-p4',boardDeviceId:'board',connectionId:'9007199254740992',capabilities:{debugBootId:'0011223344556677'}};
  const oldUsb=normalizeUsbStatusPayload(raw),newUsb=normalizeUsbStatusPayload({...raw,connectionId:'9007199254740993'});
  assert.equal(oldUsb.connectionId,'9007199254740992');assert.equal(newUsb.connectionId,'9007199254740993');
  assert.equal(usbStatusSnapshotsEqual(oldUsb,newUsb),false);
  const old=voiceRuntimeTarget(oldUsb),fresh=voiceRuntimeTarget(newUsb);assert.notEqual(old.key,fresh.key);
  let active=old.key,settle;const rows=[];
  const pending=createVoiceRuntimePoller({target:old,invoke:()=>new Promise((resolve,reject)=>{settle=rejects?reject:resolve;}),publish:r=>rows.push(r),isCurrent:()=>active===old.key}).poll();
  active=fresh.key;
  await createVoiceRuntimePoller({target:fresh,invoke:async()=>({running:true}),publish:r=>rows.push(r),isCurrent:()=>active===fresh.key}).poll();
  settle(rejects?new Error('old connection'): {running:false});await pending;
  assert.equal(rows.length,1);assert.equal(rows[0].targetKey,fresh.key);
 }
});
test('real boot capability survives normalization and invalidates same-connection target',()=>{
 const raw={connected:true,runtime:'esp-p4',boardDeviceId:'board',connectionId:'7',capabilities:{debugBootId:'0011223344556677'}};
 const before=normalizeUsbStatusPayload(raw),after=normalizeUsbStatusPayload({...raw,capabilities:{debugBootId:'8899aabbccddeeff'}});
 assert.equal(before.bootId,'0011223344556677');assert.equal(after.bootId,'8899aabbccddeeff');
 assert.equal(usbStatusSnapshotsEqual(before,after),false);
 assert.notEqual(voiceRuntimeTarget(before).key,voiceRuntimeTarget(after).key);
});
test('no native connection ID means waiting, not an invented local generation',async()=>{
 for(const connectionId of [undefined,0,1,9007199254740992,'0','01','-1','18446744073709551616','timestamp']){
  const usb=normalizeUsbStatusPayload({connected:true,runtime:'esp-p4',boardDeviceId:'board',connectionId});
  assert.equal(usb.connectionId,'');const t=voiceRuntimeTarget(usb);assert.equal(t.kind,'waiting');
  await createVoiceRuntimePoller({target:t,invoke:()=>assert.fail('missing native identity'),publish:()=>{},isCurrent:()=>true}).poll();
 }
 const disconnected=normalizeUsbStatusPayload({connected:false,connectionId:'7',bootId:'0011223344556677'});
 assert.equal(disconnected.connectionId,'');assert.equal(disconnected.bootId,'');
});
