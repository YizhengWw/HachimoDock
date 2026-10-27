import test from 'node:test';
import assert from 'node:assert/strict';
import {COMMENT_ID_KEY,COMMENT_NAME_KEY,readCommentIdentity,saveCommentName,normalizeCommentName} from './comment-identity.js';
const storage=()=>{const map=new Map();return{getItem:k=>map.get(k)??null,setItem:(k,v)=>map.set(k,v)};};
test('anonymous public code persists without borrowing hardware identity or creator name',()=>{
  const s=storage();s.setItem('pet-manager:community-display-name:v1','组件作者');
  const first=readCommentIdentity(s);assert.match(first.displayName,/^用户-[a-f0-9]{12}$/);
  assert.equal(first.displayName,readCommentIdentity(s).displayName);
  assert.equal(s.getItem(COMMENT_ID_KEY),first.code);
  assert.equal(s.getItem(COMMENT_NAME_KEY),null);
});
test('saved nickname is trimmed, durable, separate from attribution and anonymous code',()=>{
  const s=storage(),initial=readCommentIdentity(s);s.setItem('pet-manager:community-display-name:v1','原作者');
  assert.equal(saveCommentName(s,' 橘子汽水 '),'橘子汽水');
  assert.equal(readCommentIdentity(s).displayName,'橘子汽水');
  assert.equal(readCommentIdentity(s).code,initial.code);
  assert.equal(s.getItem('pet-manager:community-display-name:v1'),'原作者');
});
test('empty/overlong/control names rejected, emoji and plain text remain ordinary text',()=>{
  for(const s of ['  ','字'.repeat(41),'昵称\n换行','假\u202e名'])assert.throws(()=>normalizeCommentName(s));
  assert.equal(normalizeCommentName('橘子🍊'),'橘子🍊');
  assert.equal(normalizeCommentName('<b>我</b>'),'<b>我</b>');
});
test('blocked storage has a stable session identity but cannot pretend nickname was saved',()=>{
  const s={getItem(){throw Error('blocked');},setItem(){throw Error('blocked');}};
  assert.equal(readCommentIdentity(s).displayName,readCommentIdentity(s).displayName);
  assert.throws(()=>saveCommentName(s,'名字'),/保存失败/);
  const corrupt=storage();corrupt.setItem(COMMENT_ID_KEY,'real-device-serial');corrupt.setItem(COMMENT_NAME_KEY,'坏\n名');
  assert.match(readCommentIdentity(corrupt).displayName,/^用户-[a-f0-9]{12}$/);
});
