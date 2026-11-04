import test from 'node:test';
import assert from 'node:assert/strict';
import {SUBJECT_MOTION_POLICY_ZH,SUBJECT_MOTION_POLICY_EN,withSubjectMotionPolicy} from './motion-semantics.js';
import {ACTION_START_POSES,buildActionStartPrompt,ensureActionStartFrame} from './action-start-frame.js';
import {CUSTOM_GENERATION_FAMILIES} from './families.js';
import {buildUserPrompt,buildDryRunResponse} from './prompts.js';
import {SYSTEM_PROMPT_ZH} from './system-prompt.js';
import {reviewVisual,checkpointedReview,QUALITY_REVIEW_VERSION} from './quality.js';
import {BLACK_BACKGROUND_EDIT_PROMPT} from './providers/volcano-image.js';

test('all state first-frame designs express meaning without mandatory body parts or angles',()=>{
 assert.equal(Object.keys(ACTION_START_POSES).length,11);
 for(const [family,pose] of Object.entries(ACTION_START_POSES)){
  assert.doesNotMatch(pose,/肩部|双膝|双脚|一只脚|抬到肩|朝左|四十五度/);
  assert.ok(buildActionStartPrompt(family).includes(SUBJECT_MOTION_POLICY_ZH));
 }
 for(const p of [SYSTEM_PROMPT_ZH,BLACK_BACKGROUND_EDIT_PROMPT])assert.ok(p.includes(SUBJECT_MOTION_POLICY_ZH));
 assert.ok(buildUserPrompt({families:CUSTOM_GENERATION_FAMILIES}).includes(SUBJECT_MOTION_POLICY_EN));
 for(const item of buildDryRunResponse({families:CUSTOM_GENERATION_FAMILIES}).prompts)assert.ok(item.prompt.includes(SUBJECT_MOTION_POLICY_EN));
});

test('reference, first-frame and video review all use the same morphology policy',async()=>{
 const old=globalThis.fetch;const bodies=[];
 globalThis.fetch=async(_url,init)=>{bodies.push(JSON.parse(init.body));return Response.json({output_text:'{"pass":true,"issues":[]}'});};
 try{
  for(const kind of ['reference','action_reference','video'])await reviewVisual({thinking:{apiKey:'fixture'},images:['data:image/png;base64,AQ==','data:image/png;base64,Ag=='],kind,styleDescription:buildActionStartPrompt('welcome')});
  for(const body of bodies){
   const texts=body.input[0].content.filter(c=>c.type==='input_text').map(c=>c.text).join('\n');
   assert.ok(texts.includes(SUBJECT_MOTION_POLICY_ZH));assert.match(texts,/缺少器官.*不能单独作为不通过理由/);
   assert.equal(body.store,false);assert.equal(body.text.format.strict,true);
  }
 }finally{globalThis.fetch=old;}
});

test('explicit recovery rechecks old anatomy rejection once without regenerating paid media',async()=>{
 const old=globalThis.fetch;let reviews=0,generations=0;
 globalThis.fetch=async()=>{reviews++;return Response.json({output_text:'{"pass":true,"issues":[]}'});};
 const rejected={pass:false,issues:['没有外露手臂，无法抬肩招手']};
 const owner={startFrame:{bytes:new Uint8Array([2]),mime:'image/png',dataUrl:'data:image/png;base64,Ag=='},startFrameQuality:rejected,startFrameQualityVersion:QUALITY_REVIEW_VERSION-1,startFrameQualityIntent:true};
 const original=owner.startFrame;
 const opts={owner,actions:{welcome:owner},family:'welcome',reference:{bytes:new Uint8Array([1]),dataUrl:'data:image/png;base64,AQ=='},thinking:{apiKey:'fixture'},commit:async()=>{},generate:async()=>{generations++;throw Error('must reuse saved media');}};
 try{
  await assert.rejects(ensureActionStartFrame(opts),/首帧检查未通过/);assert.equal(reviews,0);
  assert.equal(await ensureActionStartFrame({...opts,retryReview:true}),original);
  assert.equal(reviews,1);assert.equal(generations,0);assert.equal(owner.startFrameQualityPrevious.verdict,rejected);
  assert.equal(owner.startFrameQualityVersion,QUALITY_REVIEW_VERSION);
  await ensureActionStartFrame({...opts,retryReview:true});assert.equal(reviews,1);
 }finally{globalThis.fetch=old;}
});

test('cached legacy video prompts gain the same policy without changing resume ownership',()=>{
 const legacy='Raise an arm to greet the viewer.';
 const adapted=withSubjectMotionPolicy(legacy);
 assert.ok(adapted.startsWith(legacy));assert.ok(adapted.includes(SUBJECT_MOTION_POLICY_EN));
 assert.equal(withSubjectMotionPolicy(adapted),adapted);
});


test('manual recovery migrates an old raw-only rejection in one review and retains its evidence',async()=>{
 const old=globalThis.fetch;let calls=0;
 const raw={output_text:'{"pass":false,"issues":["原形没有手臂"]}'};
 const owner={qualityResponse:raw,qualityVersion:QUALITY_REVIEW_VERSION-1,qualityIntent:true,videoBytes:new Uint8Array([7])};
 const opts={owner,key:'quality',commit:async()=>{},thinking:{apiKey:'fixture'},images:['data:image/png;base64,AQ=='],kind:'reference',retryReview:true};
 globalThis.fetch=async()=>{calls++;return Response.json({output_text:'{"pass":true,"issues":[]}'});};
 try{
  assert.equal((await checkpointedReview(opts)).pass,true);assert.equal(calls,1);
  assert.equal(owner.qualityPrevious.response,raw);assert.equal(owner.qualityPrevious.verdict.pass,false);
  assert.equal(owner.videoBytes[0],7);assert.equal(owner.qualityVersion,QUALITY_REVIEW_VERSION);
  await checkpointedReview(opts);assert.equal(calls,1);
 }finally{globalThis.fetch=old;}
});
