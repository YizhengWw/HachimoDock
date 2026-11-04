import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {keyframeDeclaration,keyframeAsset,keyframeId,keyframeIndex,keyframeImage,keyframeImageEntries} from './keyframe-background.js';
const fixture=JSON.parse(readFileSync(new URL('../../skills/petui/contracts/keyframe-bg-v1.json',import.meta.url)));
test('keyframe asset and pack-id derivations follow the shared contract',()=>{
  for(const a of fixture.assets){assert.equal(keyframeAsset(a.asset,a.frame),a.source);assert.equal(keyframeId(a.id,a.frame),a.packId);}
  assert.equal(keyframeDeclaration(fixture.valid[0]),null);
  assert.deepEqual(keyframeDeclaration(fixture.valid[2]),{id:'bg',asset:'assets/bg.png',frames:3,fps:2});
});
test('preview swaps frames with the clock, never faster than every 500 ms, and falls back to the declared image',()=>{
  const surface=fixture.valid[2],d=keyframeDeclaration(surface);
  assert.equal(keyframeIndex(d,0),0);assert.equal(keyframeIndex(d,499),0);assert.equal(keyframeIndex(d,500),1);assert.equal(keyframeIndex(d,1000),2);assert.equal(keyframeIndex(d,1500),0);
  const images=[{id:'bg',dataUrl:'a'},...keyframeImageEntries(surface.images,asset=>asset)];
  assert.deepEqual(images.slice(1).map(im=>[im.id,im.asset]),[['bg.2','assets/bg.2.png'],['bg.3','assets/bg.3.png']]);
  const node={type:'image',image:'bg'};
  assert.equal(keyframeImage(images,surface,node,0).id,'bg');assert.equal(keyframeImage(images,surface,node,600).id,'bg.2');assert.equal(keyframeImage(images,surface,node,1100).id,'bg.3');
  assert.equal(keyframeImage([{id:'bg',dataUrl:'a'}],surface,node,600).id,'bg','missing sibling falls back to the declared background');
  assert.equal(keyframeImage(images,fixture.valid[0],node,600).id,'bg','static backgrounds never cycle');
});
