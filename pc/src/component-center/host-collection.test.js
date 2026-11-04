import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {hostCollectionConfig,hostCollectionSnapshot,hostCollectionContext,hostCollectionSurface,updateCollectionReply} from './host-collection.js';
const suite=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/host-collection-v1.json',import.meta.url)));
const runtime=()=>{const r=structuredClone(suite.runtimeBase);r.surface.collection_view=structuredClone(suite.config);return r;};
function patched(value,p){const out=structuredClone(value),parts=p.path.slice(1).split('/');let parent=out;for(const k of parts.slice(0,-1))parent=parent[k];parent[parts.at(-1)]=p.value;return out;}
test('host collection shares strict bounded config and snapshot rejection vectors',()=>{
  assert.deepEqual(hostCollectionConfig(suite.config),suite.config);assert.deepEqual(hostCollectionSnapshot(suite.snapshot),suite.snapshot);
  for(const p of suite.configReject)assert.throws(()=>hostCollectionConfig(patched(suite.config,p)),p.path);
  for(const p of suite.snapshotReject)assert.throws(()=>hostCollectionSnapshot(patched(suite.snapshot,p)),p.path);
  for(const k of ['selectionKey','rows','source']){const m=structuredClone(suite.snapshot);delete m[k];assert.throws(()=>hostCollectionSnapshot(m));}
});
test('host collection expands visible rows into budgeted standard nodes without extra scalar variables',()=>{
  const r=runtime();hostCollectionConfig(r.surface.collection_view,r.surface,r);
  const surface=hostCollectionSurface(r.surface,suite.snapshot);assert.equal(surface.elements.length,8);
  assert.equal(surface.elements[3].text,'第一项');assert.equal(surface.elements[6].text,'第二项');
  assert.equal(surface.elements[5].color,suite.config.colors[1]);assert.equal(surface.elements[5]._collectionLabel,'第二项');
  assert.equal(surface.elements[5].action,'host.pick_b');assert.equal(surface.elements[5].y,164);
  assert.deepEqual(r.vars,{count:{type:'int',init:0}});assert.equal(r.surface.elements.length,2);
  r.surface.elements.push({type:'button',x:30,y:110,w:100,h:48,action:'host.other'});assert.throws(()=>hostCollectionSurface(r.surface,suite.snapshot));
});
test('empty tail and missing source publish no invented row actions or stale data',()=>{
  const s=runtime().surface,m=structuredClone(suite.snapshot),empty={present:false,key:'',label:'',detail:'',index:-1,selected:false,truncated:false};
  m.count=0;m.selectionKey='retained-offpage';m.rows=[empty,empty];
  let nodes=hostCollectionSurface(s,m).elements.slice(2);assert.deepEqual(nodes.filter(n=>!n.hidden).map(n=>n.text),['暂无条目']);assert(nodes.every(n=>!n.action));
  m.count=1;m.rows=[structuredClone(suite.snapshot.rows[0]),empty];nodes=hostCollectionSurface(s,m).elements.slice(2);
  assert.equal(nodes[0].action,'host.pick_a');assert(nodes.slice(3).every(n=>n.hidden&&!n.action));
  m.source='wrong';nodes=hostCollectionSurface(s,m).elements.slice(2);assert.deepEqual(nodes.filter(n=>!n.hidden).map(n=>n.text),['等待电脑数据']);assert(nodes.every(n=>!n.action));
});
test('host row replacement epochs survive batching and away-back while unchanged polls remain stable',()=>{
  let a=updateCollectionReply({}, {collectionView:suite.snapshot});assert.equal(a._collectionEpoch,1);
  const equal=updateCollectionReply(a,{collectionView:structuredClone(suite.snapshot)});assert.equal(equal._collectionEpoch,1);
  const changed=structuredClone(suite.snapshot);changed.rows[0].label='replacement';
  a=updateCollectionReply(a,{collectionView:changed});a=updateCollectionReply(a,{collectionView:suite.snapshot});assert.equal(a._collectionEpoch,3);
  a=updateCollectionReply(a,{ui:{mode:7,actions:[]}},true);assert.equal(a._collectionEpoch,4);assert.equal(a.collectionView,undefined);
  const overflow=updateCollectionReply({_collectionEpoch:Number.MAX_SAFE_INTEGER},{collectionView:suite.snapshot});assert.equal(overflow.collectionView,null);assert.deepEqual(overflow.ui,{mode:7,actions:[]});
});
test('host collection text limits use UTF8 bytes and invalid model is never a context',()=>{
  const m=structuredClone(suite.snapshot);m.rows[0].label='🙂'.repeat(15);assert.doesNotThrow(()=>hostCollectionSnapshot(m));
  for(const bad of ['🙂'.repeat(16),'\u0085','\ud800']){m.rows[0].label=bad;assert.throws(()=>hostCollectionSnapshot(m));assert.equal(hostCollectionContext(m),'unavailable');}
  const plain={elements:[]};assert.equal(hostCollectionSurface(plain,m),plain);
});
