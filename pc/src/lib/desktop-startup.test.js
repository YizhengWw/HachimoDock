import test from 'node:test';
import assert from 'node:assert/strict';
import {loadDesktopStartup} from './desktop-startup.js';
test('denied legacy credentials do not suppress bindings or desktop navigation',async()=>{
  let bindingsLoaded=false;
  const result=await loadDesktopStartup(async()=>{throw Error('legacy auth denied');},async()=>{bindingsLoaded=true;return [{boardDeviceId:'fixture'}];});
  assert(bindingsLoaded);assert.equal(result.bindings[0].boardDeviceId,'fixture');
  assert.equal(result.configError,'legacy auth denied');
});
test('binding failures remain explicit and credential success is not misreported',async()=>{
  await assert.rejects(loadDesktopStartup(async()=>{},async()=>{throw Error('binding read failed');}),/binding read failed/);
  assert.deepEqual(await loadDesktopStartup(async()=>{},async()=>[]),{bindings:[],configError:''});
});
test('both services start independently, even when initialization throws synchronously',async()=>{
  let called=false;
  const result=await loadDesktopStartup(()=>{throw Error('sync failure');},()=>{called=true;return [];});
  assert(called);assert.equal(result.configError,'sync failure');
});
