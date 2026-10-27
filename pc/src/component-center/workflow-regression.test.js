import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const source=name=>readFileSync(new URL(name,import.meta.url),'utf8');
test('UI confirmations stay inside app and publication sources retain custom variants',()=>{
  const community=source('CommunityCenter.jsx'),center=source('../ComponentCenter.jsx');
  assert.doesNotMatch(community,/window\.confirm/);assert.doesNotMatch(center,/window\.confirm/);
  assert.match(community,/WorkflowConfirm title="放弃未提交修改/);
  assert.match(center,/WorkflowConfirm title="设备版本更高/);
  assert.match(community,/localItems=\{publicationItems\.filter/);
  assert.match(community,/setCovers\(\[\]\);setRights\(false\);setPrivacy\(false\);setTested\(false\)/);
});
test('background refresh retains same-identity data but never reuses a different query result',()=>{
  const community=source('CommunityCenter.jsx');
  assert.match(community,/identityDependencies = dependencies/);
  assert.match(community,/data:sameRequest\?old\.data:null/);
  const publicWeb=readFileSync(new URL('../../../server/community/web/app.js',import.meta.url),'utf8');
  assert.match(publicWeb,/if\(turn!==epoch\)return/);
  assert.match(publicWeb,/key!==`\$\{state.metric\}:\$\{state.window\}`/);
});
test('transport cancellation, retries and target changes are guarded before sending',()=>{
  const center=source('../ComponentCenter.jsx');
  assert.match(center,/if\(transferInFlight\.current\)return false/);
  assert.match(center,/finally \{\s*transferInFlight\.current=false/);
  assert.match(center,/连接方式或目标已变化/);
  assert.match(center,/options\.epoch!==installEpoch\.current/);
});
