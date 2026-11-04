import test from 'node:test';
import assert from 'node:assert/strict';
import {syncAudioPreflight,deviceDeletePolicy,componentTransferError} from './component-transfer-policy.js';

test('sync audio preflight issues only a readonly correlated status request and preserves voice block',async()=>{
 const calls=[];
 const result=await syncAudioPreflight(async(command,input)=>{calls.push([command,input]);return {syncAudio:{blocked:true,reason:'voice'}};},'board-a');
 assert.equal(result.reason,'voice');
 assert.deepEqual(calls,[['usb_get_diagnostics',{expectedBoardDeviceId:'board-a',componentSync:true}]]);
});
test('unknown audio status fails closed before any installation',async()=>{
 await assert.rejects(syncAudioPreflight(async()=>({}),'board-a'),/尚未开始同步/);
 await assert.rejects(syncAudioPreflight(async()=>{throw Error('disconnected');},'board-a'),/disconnected/);
});
test('device saves default to preserve with exact advertised capability',()=>{
 assert.deepEqual(deviceDeletePolicy({transport:'usb',capabilities:{widgetDeletePreserveSave:true}}),{preserveSave:true,clearDeviceSaveConfirmed:false});
 for(const transport of ['usb','ssh'])assert.throws(()=>deviceDeletePolicy({transport,capabilities:{widgetDeletePreserveSave:'true'}}),/未删除/);
 assert.deepEqual(deviceDeletePolicy({clearSave:true,transport:'usb'}),{preserveSave:false,clearDeviceSaveConfirmed:true});
});
test('unknown commit and busy results have actionable Chinese text without claiming success',()=>{
 assert.match(componentTransferError('widget commit acknowledgement timed out'),/结果尚未确认/);
 assert.match(componentTransferError('audio or component storage is busy; staged package preserved'),/停止播放/);
 assert.match(componentTransferError('component storage is busy'),/检查设备清单/);
});
