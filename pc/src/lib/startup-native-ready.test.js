import test from 'node:test';
import assert from 'node:assert/strict';
import {createStartupHandshake} from './startup-native-ready.js';
test('captures one document generation before root mount; StrictMode confirms once',async()=>{
 const calls=[];const handshake=createStartupHandshake(async(command,args)=>{
  calls.push([command,args]);return command==='frontend_startup_generation'?{generation:3,token:'document-token'}:true;
 },()=>true);
 await handshake.capture();await handshake.capture();assert.equal(calls.length,1);
 assert.equal(await handshake.confirm(),true);assert.equal(await handshake.confirm(),true);
 assert.deepEqual(calls,[['frontend_startup_generation',undefined],['frontend_startup_ready',{generation:3,token:'document-token'}]]);
});
test('browser preview sends no native IPC',async()=>{
 const handshake=createStartupHandshake(()=>{throw Error('must not invoke');},()=>false);
 assert.equal(await handshake.capture(),null);assert.equal(await handshake.confirm(),true);
});
test('late native rejection is not retried or converted to a fresh generation',async()=>{
 let generationCalls=0,readyCalls=0;
 const handshake=createStartupHandshake(async command=>{
  if(command==='frontend_startup_generation'){generationCalls++;return {generation:1,token:'old'};}
  readyCalls++;return false;
 },()=>true);
 assert.equal(await handshake.confirm(),false);assert.equal(await handshake.confirm(),false);
 assert.equal(generationCalls,1);assert.equal(readyCalls,1);
});
test('unknown ready IPC outcome has no automatic retry',async()=>{
 let readyCalls=0;
 const handshake=createStartupHandshake(async command=>{
  if(command==='frontend_startup_generation')return {generation:1,token:'one'};
  readyCalls++;throw Error('transport unavailable');
 },()=>true);
 await assert.rejects(handshake.confirm(),/transport/);await assert.rejects(handshake.confirm(),/transport/);
 assert.equal(readyCalls,1);
});
test('invalid native challenge never sends a ready message',async()=>{
 let calls=0;const handshake=createStartupHandshake(async()=>{calls++;return {generation:-1,token:'invalid'};},()=>true);
 await assert.rejects(handshake.capture(),/invalid startup challenge/);
 await assert.rejects(handshake.confirm(),/invalid startup challenge/);assert.equal(calls,1);
});
