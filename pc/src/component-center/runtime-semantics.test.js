import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {initialApp, appAction, appAdvance} from './app-runtime.js';
import * as packaged from '../../skills/petui/scripts/app-runtime.mjs';

const suite=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/runtime-semantics-v1.json',import.meta.url)));
assert.equal(suite.schemaVersion,1);
for(const [port,model] of [['preview',{initialApp,appAction,appAdvance}],['packaged',packaged]])
for(const item of suite.cases)test(`runtime contract (${port}): ${item.name}`,()=>{
 let state=model.initialApp(item.runtime);
 for(const event of item.events)state=event.action?model.appAction(item.runtime,state,event.action):model.appAdvance(item.runtime,state,event.ms);
 assert.deepEqual({state:state.state,page:state.page,vars:state.vars},item.expected);
});
test('packaged runtime is an exact import-extension translation of preview',()=>{
 const preview=readFileSync(new URL('./app-runtime.js',import.meta.url),'utf8');
 const packaged=readFileSync(new URL('../../skills/petui/scripts/app-runtime.mjs',import.meta.url),'utf8');
 assert.equal(packaged.replaceAll('.mjs','.js'),preview);
});
