import test from 'node:test';
import assert from 'node:assert/strict';
import {ACTION_START_POSES,buildActionStartPrompt,ensureActionStartFrame} from './action-start-frame.js';
import {CUSTOM_GENERATION_FAMILIES} from './families.js';
import {buildVolcanoTaskPayload,runVolcanoFamily} from './providers/volcano.js';
import {canAutoResume} from './session.js';

test('all eleven actions have different pose designs; start frames are not used as locked ending frames',()=>{
 assert.equal(Object.keys(ACTION_START_POSES).length,11);assert.equal(new Set(Object.values(ACTION_START_POSES)).size,11);
 for(const f of CUSTOM_GENERATION_FAMILIES)assert.match(buildActionStartPrompt(f.family),/不能简单复制参考图的站姿/);
 for(const model of ['doubao-seedance-2-0-fast-260128','doubao-seedance-1-5-pro-251215']){
  const payload=buildVolcanoTaskPayload({model,prompt:'move',imageDataUrl:'data:image/png;base64,AQ==',firstFrameOnly:true});
  assert.deepEqual(payload.content.filter(c=>c.type==='image_url').map(c=>c.role),['first_frame']);
 }
});

test('raw action frame and review survive recovery; duplicate clicks/recovery do not buy another image',async()=>{
 const old=globalThis.fetch;let posts=0,reviews=0;const owner={},actions={welcome:owner};
 globalThis.fetch=async()=>{reviews++;return Response.json({output_text:'{"pass":true,"issues":[]}'});};
 const options={owner,actions,family:'welcome',reference:{bytes:new Uint8Array([1]),dataUrl:'data:image/png;base64,AQ==',master:{bytes:new Uint8Array([3]),mime:'image/png'}},cloudBackground:{apiKey:'fixture'},thinking:{apiKey:'fixture'},commit:async()=>{},generate:async args=>{assert.equal(args.imageDataUrl,'data:image/png;base64,Aw==');posts++;return {bytes:new Uint8Array([2]),mime:'image/png'};},normalize:async raw=>raw.bytes};
 try{
  const result=await ensureActionStartFrame(options);assert.equal(posts,1);assert.equal(reviews,1);assert.equal(owner.startFrameRaw.bytes[0],2);
  assert.equal(await ensureActionStartFrame(options),result);assert.equal(posts,1);assert.equal(reviews,1);
  delete owner.startFrame;delete owner.startFrameApproved;
  await ensureActionStartFrame(options);assert.equal(posts,1);assert.equal(reviews,1);
 }finally{globalThis.fetch=old;}
});

test('unknown image submissions, rejected checks and repeated poses stop before video without automatic charging',async()=>{
 let posts=0;const opts={actions:{},family:'done',reference:{bytes:new Uint8Array([1]),dataUrl:'data:image/png;base64,AQ=='},cloudBackground:{apiKey:'fixture'},thinking:{apiKey:'fixture'},commit:async()=>{},generate:async()=>{posts++;return {bytes:new Uint8Array([1]),mime:'image/png'};},normalize:async r=>r.bytes};
 await assert.rejects(ensureActionStartFrame({...opts,owner:{startFrameIntent:true}}),/结果未知/);assert.equal(posts,0);
 const owner={};await assert.rejects(ensureActionStartFrame({...opts,owner}),/完全重复/);assert.equal(posts,1);
 await assert.rejects(ensureActionStartFrame({...opts,owner}),/完全重复/);assert.equal(posts,1);
 for(const action of [{startFrameIntent:true},{startFrameQualityIntent:true,startFrameRaw:{}},{startFrameQuality:{pass:false}}])
  assert.equal(canAutoResume({automatic:true,actions:{done:action}}),false);
});

test('action-specific first-frame role reaches the actual video submit request',async()=>{
 const old=globalThis.fetch;const bodies=[];
 globalThis.fetch=async(url,init={})=>{
  if(init.method==='POST'){bodies.push(JSON.parse(init.body));return Response.json({id:'fixture-frame-video'});}
  if(url.includes('/tasks/'))return Response.json({status:'succeeded',content:{video_url:'https://fixture.invalid/video'}});
  return new Response(new Uint8Array([1]));
 };
 try{await runVolcanoFamily({config:{apiKey:'fixture',model:'doubao-seedance-2-0-fast-260128',firstFrameOnly:true},prompt:'move',imageDataUrl:'data:image/png;base64,Ag=='});
 assert.equal(bodies.length,1);assert.deepEqual(bodies[0].content.filter(c=>c.type==='image_url').map(c=>c.role),['first_frame']);
 }finally{globalThis.fetch=old;}
});
