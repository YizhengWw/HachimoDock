import test from 'node:test';
import assert from 'node:assert/strict';
import {COMPONENT_AUTHORS,componentDisplayAuthor} from './component-authors.js';
test('maintained components use pen names without rewriting third-party authors',()=>{
  assert.equal(componentDisplayAuthor({id:'stock-watchlist',official:true,author:'官方'}),'cobi123');
  assert.equal(componentDisplayAuthor({id:'stock-watchlist',author:'真实社区作者'}),'真实社区作者');
  assert.equal(componentDisplayAuthor({id:'hachimi-home',official:true,author:'哈基米'}),'哈基米');
  assert.equal(Object.keys(COMPONENT_AUTHORS).length,22);
  assert.ok(Object.values(COMPONENT_AUTHORS).every(s=>!s.includes('官方')));
});
