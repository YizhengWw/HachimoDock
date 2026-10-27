/** Whole orchestration regression; synthetic media decoder and mock network, never paid calls. */
import test from 'node:test';
import assert from 'node:assert/strict';
import {runAvatarPipeline} from './run.js';
import {CUSTOM_GENERATION_FAMILIES} from './families.js';

test('fresh distinct-frame pipeline submits its cached action pose, preserves frame metadata, and restart does not charge again',async()=>{
 const old={fetch:globalThis.fetch,Image:globalThis.Image,document:globalThis.document};let imagePosts=0,reviews=0,videoPosts=0;
 class ActionMedia extends EventTarget{duration=5;videoWidth=400;videoHeight=300;load(){if(this.src)queueMicrotask(()=>this.dispatchEvent(new Event('loadeddata')));}set currentTime(_){queueMicrotask(()=>this.dispatchEvent(new Event('seeked')));}removeAttribute(){this.src='';}}
 globalThis.Image=class{naturalWidth=400;naturalHeight=300;set src(_){queueMicrotask(()=>this.onload?.());}};
 const ctx={clearRect(){},fillRect(){},drawImage(){},getImageData:()=>({data:new Uint8Array([255,255,255,255])}),putImageData(){}};
 globalThis.document={createElement:type=>type==='video'?new ActionMedia():{getContext:()=>ctx,toBlob:cb=>cb(new Blob([new Uint8Array([9])])),toDataURL:()=> 'data:image/png;base64,CQ=='}};
 globalThis.fetch=async(url,init={})=>{
  if(url.endsWith('/images/generations')){imagePosts++;const b=JSON.parse(init.body);assert.match(b.prompt,/起始关键帧/);assert.equal(b.image[0],'data:image/png;base64,Aw==');return Response.json({data:[{b64_json:'Ag=='}]});}
  if(url.endsWith('/responses')){reviews++;return Response.json({output_text:'{"pass":true,"issues":[]}'});}
  if(init.method==='POST'){videoPosts++;const b=JSON.parse(init.body);assert.deepEqual(b.content.filter(c=>c.type==='image_url').map(c=>[c.role,c.image_url.url]),[['first_frame','data:image/png;base64,CQ==']]);return Response.json({id:'action-frame-id'});}
  if(url.includes('/tasks/'))return Response.json({status:'succeeded',content:{video_url:'https://fixture.invalid/action-video'}});
  return new Response(new Uint8Array([8]));
 };
 const session={referenceApproved:true,trialApproved:true,reference:{bytes:new Uint8Array([1]),dataUrl:'data:image/png;base64,AQ==',mime:'image/png',master:{bytes:new Uint8Array([3]),mime:'image/png'}},actions:{},manifest:{entries:[{family:'welcome',prompt:'wave naturally'}]}};
 const args={imageFile:new File(['x'],'fixture.png'),providerConfig:{provider:'volcengine',apiKey:'fixture',model:'doubao-seedance-2-0-fast-260128'},session,checkpoint:async()=>{},automatic:true,distinctStartFrames:true,familyIds:['welcome']};
 try{
  const result=await runAvatarPipeline(args);assert.equal(result.families[0].ok,true);assert.equal(imagePosts,1);assert.equal(reviews,1);assert.equal(videoPosts,1);
  assert.equal(session.actions.welcome.startFrame.bytes[0],9);assert.equal(session.actions.welcome.startFrameApproved,true);assert.equal(session.actions.welcome.taskId,'action-frame-id');
  await runAvatarPipeline(args);assert.equal(imagePosts,1);assert.equal(reviews,1);assert.equal(videoPosts,1);
 }finally{Object.assign(globalThis,old);}
});

test('a chosen preview is checked in the custom style and reused without a second image submission',async()=>{
 const old={fetch:globalThis.fetch,Image:globalThis.Image,document:globalThis.document};let calls=0;
 globalThis.Image=class {naturalWidth=256;naturalHeight=256;set src(_){queueMicrotask(()=>this.onload?.());}};
 class PreviewMedia extends EventTarget {
  duration=5;videoWidth=384;videoHeight=288;
  load(){if(this.src)queueMicrotask(()=>this.dispatchEvent(new Event('loadeddata')));}
  set currentTime(_){queueMicrotask(()=>this.dispatchEvent(new Event('seeked')));}
  removeAttribute(){this.src='';}
 }
 globalThis.document={createElement:type=>type==='video'?new PreviewMedia():({getContext:()=>({drawImage(){},getImageData:()=>({data:new Uint8Array([255,255,255,255])})}),toDataURL:()=> 'data:image/png;base64,AQ=='})};
 globalThis.fetch=async(url,init)=>{assert.match(url,/responses$/);calls++;assert.match(init.body,/水彩/);return Response.json({output_text:'{"pass":true,"issues":[]}'});};
 const reference={bytes:new Uint8Array([8]),mime:'image/png',dataUrl:'data:image/png;base64,CA==',base64:'CA=='};
 const session={reference,trialApproved:true,actions:{welcome:{videoBytes:new Uint8Array([1]),taskId:'already-known'}},manifest:{entries:[{family:'welcome',prompt:'same reference style'}]}};
 try{
  const result=await runAvatarPipeline({imageFile:new File(['fixture'],'fixture.png'),styleDescription:'水彩',providerConfig:{provider:'volcengine',apiKey:'fixture',model:'doubao-seedance-2-0-fast-260128'},session,automatic:true,checkpoint:async()=>{},familyIds:['welcome']});
  assert.equal(calls,1);assert.equal(session.referenceApproved,true);assert.equal(result.imagePayload,reference);assert.equal(result.families[0].ok,true);
 }finally{Object.assign(globalThis,old);}
});

test('completed malformed prompt can be explicitly replaced once; unknown prompt intent is not replayed',async()=>{
 const old=globalThis.fetch;let calls=0;
 globalThis.fetch=async url=>{assert.match(url,/responses$/);calls++;return Response.json({output_text:JSON.stringify({persona:{},prompts:[{family:'welcome',prompt:'same subject waves'}]})});};
 const session={thinkingIntent:true,thinkingResponse:{output_text:'{broken:true}'},actions:{welcome:{taskId:'existing',videoBytes:new Uint8Array([1])}},
  reference:{bytes:new Uint8Array([1]),mime:'image/png',dataUrl:'data:image/png;base64,AQ=='}};
 const args={imageFile:new File([new Uint8Array([1])],'fixture.png',{type:'image/png'}),providerConfig:{provider:'volcengine',apiKey:'fixture',model:'doubao-seedance-2-0-fast-260128'},session,checkpoint:async()=>{},skipProcessing:true,familyIds:['welcome']};
 try{
   await assert.rejects(runAvatarPipeline(args),e=>e.code==='PROMPT_OUTPUT_INVALID');assert.equal(calls,0);
   const result=await runAvatarPipeline({...args,retryReview:true});assert.equal(calls,1);assert.equal(result.families[0].ok,true);assert.ok(session.thinkingPreviousResponse);
   await runAvatarPipeline({...args,retryReview:true});assert.equal(calls,1);
   delete session.manifest;delete session.thinkingResponse;
   await assert.rejects(runAvatarPipeline({...args,retryReview:true}),/上次提示词生成结果未知/);assert.equal(calls,1);
 }finally{globalThis.fetch=old;}
});

test('bad trial JSON retains clip; explicit recovery generates only remaining 10; restart submits nothing',async()=>{
  const old={fetch:globalThis.fetch,document:globalThis.document,Image:globalThis.Image};
  class Media extends EventTarget {
    duration=5;videoWidth=384;videoHeight=288;
    load(){if(this.src)queueMicrotask(()=>this.dispatchEvent(new Event('loadeddata')));}
    set currentTime(_){queueMicrotask(()=>this.dispatchEvent(new Event('seeked')));}
    removeAttribute(){this.src='';}
  }
  const canvas={getContext:()=>({drawImage(){},getImageData:()=>({data:new Uint8Array([255,200,0,255])})}),toDataURL:()=> 'data:image/png;base64,AA=='};
  globalThis.document={createElement:type=>type==='video'?new Media():{...canvas}};
  globalThis.Image=class {naturalWidth=384;naturalHeight=288;set src(_){queueMicrotask(()=>this.onload?.());}};
  let reviewCalls=0,videoCalls=0;const ids=[];
  globalThis.fetch=async(url,init={})=>{
    if(url.endsWith('/responses')){reviewCalls++;return Response.json({output_text:reviewCalls===1?'{pass: true, issues: []}':'{"pass":true,"issues":[]}'});}
    if(url.includes('/contents/generations/tasks')){
      if(init.method==='POST'){videoCalls++;const id='mock-'+videoCalls;ids.push(id);return Response.json({id});}
      return Response.json({status:'succeeded',content:{video_url:'https://fixture.invalid/video'}});
    }
    if(url==='https://fixture.invalid/video')return new Response(new Uint8Array([1,2,3]));
    throw Error('unexpected network');
  };
  const session={automatic:true,actions:{},referenceApproved:true,
    reference:{bytes:new Uint8Array([1]),dataUrl:'data:image/png;base64,AA==',base64:'AA==',mime:'image/png'},
    manifest:{persona:{},entries:CUSTOM_GENERATION_FAMILIES.map(f=>({family:f.family,prompt:'same character moves'}))}};
  const args={imageFile:new File([new Uint8Array([1])],'fixture.png',{type:'image/png'}),
    providerConfig:{provider:'volcengine',apiKey:'fixture',model:'doubao-seedance-2-0-fast-260128'},automatic:true,session,
    checkpoint:async()=>{structuredClone(session);}};
  try{
    await assert.rejects(runAvatarPipeline(args),/自动检查返回格式异常/);
    assert.equal(videoCalls,1);assert.equal(reviewCalls,1);
    const first=CUSTOM_GENERATION_FAMILIES[0].family;
    assert.equal(session.actions[first].videoBytes.byteLength,3);assert.ok(session.actions[first].qualityResponse);
    await assert.rejects(runAvatarPipeline(args));assert.equal(videoCalls,1);assert.equal(reviewCalls,1);
    // Old macOS checkpoints can lose the File backing blob on relaunch;
    // approved reference + saved video must remain recoverable without new edits.
    delete session.original;
    args.imageFile={size:3,name:'legacy.jpg',type:'image/jpeg',arrayBuffer:async()=>{throw new DOMException('The object can not be found here.','NotFoundError');}};
    const result=await runAvatarPipeline({...args,retryReview:true});
    assert.equal(session.originalUnavailable,true);assert.equal(result.originalImagePayload,null);
    assert.equal(result.families.filter(f=>f.ok).length,11);assert.equal(videoCalls,11);assert.equal(new Set(ids).size,11);assert.equal(reviewCalls,2);
    await runAvatarPipeline({...args,retryReview:true});assert.equal(videoCalls,11);assert.equal(reviewCalls,2);
  } finally {Object.assign(globalThis,old);}
});
