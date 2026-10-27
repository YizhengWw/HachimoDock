import test from 'node:test';
import assert from 'node:assert/strict';
import {compareVersions,devicePackageState,readCenterPreference} from './catalog-state.js';
import {mergeComponentCatalog} from './library-order.js';
import {versionState} from './community-model.js';

test('catalog semver supports prerelease and metadata without guessing unknown versions',()=>{
  for(const [a,b,want] of [['1.10.0','1.9.9',1],['1.0.0-rc.10','1.0.0-rc.2',1],['1.0.0','1.0.0-rc.1',1],['1.0.0+local','1.0.0+cloud',0],['1.0.0-alpha','1.0.0-alpha.1',-1],['1.0.0-2','1.0.0-alpha',-1],['dev','1.0.0',null],['1.0.0-01','1.0.0',null]])assert.equal(compareVersions(a,b),want);
});
test('newer bundled default preserves local source and custom newer revisions',()=>{
  const local={id:'demo',version:'1.0.0',isLocal:true},builtin={id:'demo',version:'1.1.0'};
  const [selected]=mergeComponentCatalog([local],[builtin]);
  assert.equal(selected.version,'1.1.0');assert.deepEqual(selected.sourceAlternatives,[local,builtin]);
  assert.equal(mergeComponentCatalog([{...local,version:'2.0.0'}],[builtin])[0].isLocal,true);
  assert.equal(mergeComponentCatalog([local],[{...builtin,valid:false}])[0].isLocal,true);
});
test('device states distinguish stale, device-only, updates, unknown and newer',()=>{
  const component={id:'demo',version:'1.0.0'};
  const state=(version,freshness='live',extra={})=>devicePackageState({...component,...extra},{freshness,items:[{id:'demo',version}]});
  assert.equal(state('1.0.0','stale').key,'unknown');
  assert.equal(state('1.0.0','live',{isDeviceOnly:true}).action,'查看设备组件');
  assert.equal(state('0.9.0').key,'update');assert.equal(state('2.0.0').key,'newer');
  assert.equal(state('').key,'unknown');assert.equal(state('1.0.0').label,'已安装 1.0.0');
});
test('same-version content conflict blocks accidental replacement',()=>{
  assert.equal(versionState({version:'1.0.0',contentHash:'abcd1234'},{version:'1.0.0',versionHash:'abcd'},{}).contentConflict,false);
  assert.equal(versionState({version:'1.0.0',contentHash:'abcd1234'},{version:'1.0.0',versionHash:'ffff'},{}).contentConflict,true);
});
test('corrupt view preferences cannot crash search or hide all tabs',()=>{
  globalThis.localStorage={getItem:()=>'{"wrong":true}'};
  assert.equal(readCenterPreference('query',''),'');
  globalThis.localStorage={getItem:()=>'"nonexistent"'};
  assert.equal(readCenterPreference('tab','discover'),'discover');
  delete globalThis.localStorage;
});
