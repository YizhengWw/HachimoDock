import test from 'node:test';
import assert from 'node:assert/strict';
import {appTile,appVisible,appHit} from './app-runtime.js';
test('atlas wraps choices and clamps measured growth independently',()=>{
  const n={tile:{columns:3,rows:3,row:2,var:'elapsed'}};
  assert.equal(appTile(n,{elapsed:4}).index,1);
  assert.equal(appTile(n,{elapsed:-1}).index,2);
  n.tile.max_var='duration';
  for(const [v,expected] of [[0,0],[99,0],[100,1],[200,2],[300,2],[1e9,2],[-1,0]])assert.equal(appTile(n,{elapsed:v,duration:300}).index,expected);
  assert.equal(appTile(n,{elapsed:1,duration:0}).index,0);
});
test('hidden harvests cannot be touched; last visible action wins',()=>{
  const base={x:20,y:20,w:60,h:60,action:'under'};
  const n={...base,action:'harvest',page:1,show:{var:'flowers',min:1,max:3}};
  assert.equal(appVisible(n,1,{flowers:0}),false);
  assert.equal(appHit({elements:[base,n]},1,30,30,{flowers:0}),'under');
  assert.equal(appHit({elements:[base,n]},1,30,30,{flowers:1}),'harvest');
  assert.equal(appHit({elements:[n]},0,30,30,{flowers:1}),null);
  assert.equal(appHit({elements:[n]},1,80,30,{flowers:1}),null);
});
