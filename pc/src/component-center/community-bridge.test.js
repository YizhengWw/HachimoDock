import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {parseCommunityMessage,COMMUNITY_ORIGIN} from './community-bridge.js';
const source={},channel='session';
function message(action,fields){return {origin:COMMUNITY_ORIGIN,source,data:{v:1,type:'pet-community-request',channel,requestId:'web-1',action,fields}};}
test('community bridge requires exact origin, source and session',()=>{
  const e=message('detail',{id:'sample-component'});assert.ok(parseCommunityMessage(e,source,COMMUNITY_ORIGIN,channel));
  for(const wrong of [{...e,source:{}},{...e,origin:'https://evil.example'},{...e,data:{...e.data,channel:'other'}},{...e,data:{...e.data,v:2}}])assert.equal(parseCommunityMessage(wrong,source,COMMUNITY_ORIGIN,channel),null);
});
test('community bridge rejects native commands, paths, URLs, credentials and oversized inputs',()=>{
  for(const [a,f] of [['publish',{path:'/private'}],['prepare',{}],['download',{id:'sample-component',path:'/private'}],['download',{id:'../../etc'}],['detail',{id:'sample-component',url:'http://127.0.0.1'}],['comment',{id:'sample-component',text:'x'.repeat(501),requestId:'x'.repeat(16)}],['profile',{name:'x'.repeat(41)}],['browse',{offset:-1}],['like',{id:'sample-component',active:'true'}]])assert.equal(parseCommunityMessage(message(a,f),source,COMMUNITY_ORIGIN,channel),null);
});
test('community bridge preserves only operation-specific public fields',()=>{
  assert.deepEqual(parseCommunityMessage(message('browse',{query:'电台',view:'discover',window:'7d'}),source,COMMUNITY_ORIGIN,channel).fields,{query:'电台'});
  const capability=JSON.parse(readFileSync(new URL('../../src-tauri/capabilities/default.json',import.meta.url)));assert.equal(capability.remote,undefined);
  assert.ok(capability.permissions.includes('local-app-commands'));
  const permission=JSON.parse(readFileSync(new URL('../../src-tauri/permissions/local-app-commands.json',import.meta.url))).permission[0];
  const rust=readFileSync(new URL('../../src-tauri/src/lib.rs',import.meta.url),'utf8');
  const commands=rust.split('tauri::generate_handler![')[1].split('])')[0].split(',').map(x=>x.trim().split('::').at(-1)).filter(Boolean);
  assert.deepEqual(permission.commands.allow,commands,'every registered app command must require the local-only capability');
  const config=JSON.parse(readFileSync(new URL('../../src-tauri/tauri.conf.json',import.meta.url)));assert.match(config.app.security.csp,/frame-src https:\/\/115\.191\.67\.48;/);
});
