import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {timelineValue,validImageRef,serviceImage,bridgeSnapshot,sliderData,validateBridgeSurface} from './service-bridge.js';
const suite=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/service-bridge-v1.json',import.meta.url)));
test('shared timeline vectors freeze, clamp, expire and reject zero duration',()=>{
 for(const v of suite.timelineVectors)assert.equal(timelineValue(v.timeline,v.age),v.value);
});
test('display interpolation is bounded and never mutates the authoritative snapshot',()=>{
 const snapshot={vars:{progress:10,title:'真实数据'},_bridgeReceivedAt:1000,presentation:{timelines:{progress:suite.timelineVectors[0].timeline}},ui:{mode:0,actions:['host.level']}};
 assert.equal(bridgeSnapshot(suite.runtime,snapshot,4000).vars.progress,12);
 assert.equal(snapshot.vars.progress,10);
 assert.deepEqual(bridgeSnapshot(suite.runtime,snapshot,11000).ui.actions,[]);
});
test('dynamic image lookup requires exact key and revision and accepts only decoded PNG previews',()=>{
 const r={key:'asset-a',revision:'version-1',width:192,height:192};assert(validImageRef(r));
 const s={presentation:{images:{cover:r}},serviceImages:{cover:{...r,dataUrl:'data:image/png;base64,AA=='}}};
 assert(serviceImage({image_bind:'cover'},s));s.serviceImages.cover.revision='old';assert.equal(serviceImage({image_bind:'cover'},s),null);
 assert(!validImageRef({...r,width:193}));assert(!validImageRef({...r,url:'https://example.com/'}));
});
test('one slider primitive covers volume, seek percent and bounded signed settings',()=>{
 const base={type:'meter',x:20,w:400,value_param:'amount'};
 assert.deepEqual(sliderData(base,220),{amount:50});
 assert.deepEqual(sliderData({...base,value_min:-10,value_max:40},220),{amount:15});
 assert.deepEqual(sliderData(base,-100),{amount:0});assert.deepEqual(sliderData(base,1000),{amount:100});
 assert.equal(sliderData({...base,value_param:'../escape'},20),null);
});
test('extension admission requires explicit host context and cannot bypass confirmation',()=>{
 assert.deepEqual(validateBridgeSurface(suite.runtime.surface,suite.runtime),[]);
 const r=structuredClone(suite.runtime);r.input.confirm={'host.level':'Confirm'};assert(validateBridgeSurface(r.surface,r).length);
 const s=structuredClone(suite.runtime.surface);delete s.services;assert(validateBridgeSurface(s,suite.runtime).length);
});
