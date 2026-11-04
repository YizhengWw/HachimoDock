import test from 'node:test';
import assert from 'node:assert/strict';
import {BACKGROUND_IDLE,backgroundSnapshot,createProgramBackgroundController,supportsBackgroundPrograms,supportsBackgroundProgramEngine} from './program-background.js';
const bootId='0123456789abcdef';
const row={widgetId:'focus',registration:'18446744073709551615',live:true,stopped:false,checkpointPending:true,revision:1,phase:1,lastStepUs:50,maxStepUs:100};
const snapshot=(items=[row])=>({bootId,items});
function deferred(){let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return {promise,resolve,reject};}
test('background capabilities are closed on ordinary and older firmware',()=>{
  assert.equal(supportsBackgroundPrograms(),false);
  const caps={widgetProgram:'p4-component-program-v1',widgetProgramBackground:'p4-program-background-v2',widgetProgramInspect:'p4-program-inspect-v1',widgetProgramBackgroundMaxActive:1};
  assert.equal(supportsBackgroundPrograms(caps),true);assert.equal(supportsBackgroundPrograms({...caps,widgetProgramInspect:null}),false);
  assert.equal(supportsBackgroundPrograms({...caps,widgetProgramBackgroundMaxActive:1.5}),false);
});
test('r158 explicitly negotiates v2 plus v1 compatibility while retaining background/inspect/count gates',()=>{
  // Exact relevant projection of r159-fix-all-ojrsrcfy/r158-actual-capabilities.json.
  const caps={widgetProgram:'p4-component-program-v2',widgetPrograms:['p4-component-program-v1','p4-component-program-v2'],
    widgetProgramBackground:'p4-program-background-v2',widgetProgramInspect:'p4-program-inspect-v1',widgetProgramBackgroundMaxActive:1,
    widgetProgramMaxFuel:4096,widgetProgramAggregateFuel:4096,widgetProgramParent:'p4-program-parent-v1'};
  assert.equal(supportsBackgroundPrograms(caps),true);
  const {widgetPrograms,...scalar}=caps;assert.equal(supportsBackgroundPrograms(scalar),true);
  assert.equal(supportsBackgroundPrograms({...caps,widgetProgram:'p4-component-program-v1'}),true);
  for(const change of [{widgetProgram:null},{widgetProgram:['p4-component-program-v1','p4-component-program-v2']},
    {widgetProgram:'p4-component-program-v3'},{widgetPrograms:null},{widgetPrograms:[]},{widgetPrograms:'p4-component-program-v2'},
    {widgetPrograms:['p4-component-program-v1']},{widgetPrograms:['p4-component-program-v2','p4-component-program-v2']},
    {widgetPrograms:['p4-component-program-v2','p4-component-program-v3']},{widgetPrograms:[true,'p4-component-program-v2']},
    {widgetProgramBackground:'p4-program-background-v1'},{widgetProgramInspect:null},{widgetProgramBackgroundMaxActive:0},
    {widgetProgramBackgroundMaxActive:5},{widgetProgramBackgroundMaxActive:'1'},{widgetProgramBackgroundMaxActive:1.5}]){
    assert.equal(supportsBackgroundPrograms({...caps,...change}),false,JSON.stringify(change));
  }
  assert.equal(supportsBackgroundPrograms(null),false);
});
test('a program entry requires its own explicitly negotiated engine, not just another known version',()=>{
  const caps={widgetProgram:'p4-component-program-v2',widgetProgramBackground:'p4-program-background-v2',widgetProgramInspect:'p4-program-inspect-v1',widgetProgramBackgroundMaxActive:1};
  assert.equal(supportsBackgroundProgramEngine(caps,'p4-component-program-v2'),true);
  assert.equal(supportsBackgroundProgramEngine(caps,'p4-component-program-v1'),false);
  const both={...caps,widgetPrograms:['p4-component-program-v1','p4-component-program-v2']};
  assert.equal(supportsBackgroundProgramEngine(both,'p4-component-program-v1'),true);
  assert.equal(supportsBackgroundProgramEngine(both,'p4-component-program-v3'),false);
  assert.equal(supportsBackgroundProgramEngine(both,['p4-component-program-v2']),false);
  assert.equal(supportsBackgroundProgramEngine(null,'p4-component-program-v2'),false);
});
test('background snapshots retain exact u64 strings and discard foreign fields',()=>{
  const value=backgroundSnapshot({...snapshot([{...row,privatePath:'never-reflect'}]),apiKey:'never-reflect'});
  assert.equal(value.items[0].registration,row.registration);assert.equal(JSON.stringify(value).includes('never-reflect'),false);
  for(const invalid of ['0','01','-1','18446744073709551616',9007199254740992])assert.throws(()=>backgroundSnapshot(snapshot([{...row,registration:invalid}])));
  for(const change of [{phase:256},{revision:1.5},{stopped:true},{checkpointPending:1},{lastStepUs:101}])assert.throws(()=>backgroundSnapshot(snapshot([{...row,...change}])));
  assert.throws(()=>backgroundSnapshot(snapshot([row,row])));
  for(const widgetId of ['Upper','has.dot','1first','a'.repeat(48)])assert.throws(()=>backgroundSnapshot(snapshot([{...row,widgetId}])));
  assert.equal(backgroundSnapshot(snapshot([{...row,widgetId:'a'.repeat(47)}])).items[0].widgetId.length,47);
});
test('one read flight, no automatic start, and disposal never publishes obsolete replies',async()=>{
  const wait=deferred(),calls=[],views=[];
  const c=createProgramBackgroundController({board:'p4-test',invoke:(...args)=>{calls.push(args);return wait.promise;},onChange:s=>views.push(s)});
  assert.equal(c.getState(),BACKGROUND_IDLE);assert.equal(calls.length,0);
  const a=c.refresh(),b=c.refresh();await Promise.resolve();assert.equal(calls.length,1);assert.equal(calls[0][0],'usb_get_background_programs');
  c.dispose();const count=views.length;wait.resolve(snapshot());await Promise.all([a,b]);assert.equal(views.length,count);
  await assert.rejects(c.start('focus'),/disposed/);assert.equal(calls.length,1);
});
test('one mutation at a time and unknown outcome requires explicit read, never replay',async()=>{
  const wait=deferred(),calls=[];
  const c=createProgramBackgroundController({board:'p4-test',invoke:(name,args)=>{calls.push([name,args]);return name==='usb_get_background_programs'?snapshot():wait.promise;}});
  await c.refresh();const started=c.start('focus');await assert.rejects(c.start('focus'),/not_ready/);
  await c.refresh();assert.equal(calls.length,2);wait.resolve({confirmed:false,code:'private-ignored'});await started;
  assert.equal(c.getState().status,'unknown');await assert.rejects(c.start('focus'),/not_ready/);assert.equal(calls.length,2);
  await c.refresh();assert.equal(c.getState().status,'ready');assert.equal(calls.filter(([name])=>name==='usb_start_background_program').length,1);
});
test('stop uses the observed epoch/serial, not a replacement current owner',async()=>{
  const calls=[];const c=createProgramBackgroundController({board:'p4-test',invoke:(name,args)=>{calls.push([name,args]);return name==='usb_get_background_programs'?snapshot():{confirmed:true,snapshot:snapshot([{...row,live:false,stopped:true}])};}});
  await c.refresh();await assert.rejects(c.stop('focus',{bootId:'1123456789abcdef',registration:row.registration}),/owner_changed/);
  await assert.rejects(c.stop('focus',{bootId,registration:'1'}),/owner_changed/);assert.equal(calls.length,1);
  await c.stop('focus',{bootId,registration:row.registration});assert.equal(calls[1][1].registration,row.registration);assert.equal(c.getState().snapshot.items[0].checkpointPending,true);
});
test('invalid or failed confirmation stays unknown without private error reflection',async()=>{
  for(const result of [{confirmed:true,snapshot:{bootId,items:[]}}, {confirmed:true,snapshot:snapshot([{...row,live:false,stopped:true}])}]){
    const c=createProgramBackgroundController({board:'p4-test',invoke:name=>name==='usb_get_background_programs'?snapshot():result});
    await c.refresh();await c.start('focus');assert.equal(c.getState().status,'unknown');
  }
  const c=createProgramBackgroundController({board:'p4-test',invoke:name=>{if(name==='usb_get_background_programs')return snapshot();throw Error('private-path-key');}});
  await c.refresh();await c.start('focus');assert.equal(JSON.stringify(c.getState()).includes('private-path-key'),false);
});
