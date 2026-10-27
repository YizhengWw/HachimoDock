import {test} from 'node:test';
import assert from 'node:assert/strict';
import {refreshGate} from './community-refresh.js';
test('focus and visibility share one bounded refresh clock',()=>{
  let now=100;const refresh=refreshGate(60_000,()=>now);
  assert.equal(refresh(),false);now+=60_000;assert.equal(refresh(),true);
  assert.equal(refresh(),false);assert.equal(refresh(true),true);
  now=0;assert.equal(refresh(),true);
});
