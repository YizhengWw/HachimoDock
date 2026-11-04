import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {parseQualityVerdict,reviewVisual,isRecoverableQueryError,checkpointedReview,inspectReference,inspectVideo,videoInspectionFailure,settleVideoFrame,QUALITY_REVIEW_VERSION} from './quality.js';
import {getGenerationDiagnostics,formatDiagnostic} from './diagnostics.js';
import {canAutoResume} from './session.js';
import {CUSTOM_GENERATION_FAMILIES} from './families.js';
import {BLACK_BACKGROUND_EDIT_PROMPT} from './providers/volcano-image.js';

test('background rendering wait is bounded and responds to cancellation',async()=>{
 const old=globalThis.requestAnimationFrame;globalThis.requestAnimationFrame=()=>1;
 try{await settleVideoFrame();await assert.rejects(settleVideoFrame(AbortSignal.abort()),e=>e.name==='AbortError');}
 finally{globalThis.requestAnimationFrame=old;}
});

test('restored image handles are rehydrated from complete bytes before object-URL decoding',async()=>{
 const old={Image:globalThis.Image,document:globalThis.document,create:URL.createObjectURL,revoke:URL.revokeObjectURL};
 const file=new File([new Uint8Array([1,2,3])],'restored.jpg',{type:'image/jpeg'});let owned;
 URL.createObjectURL=blob=>{owned=blob;return 'blob:fixture';};URL.revokeObjectURL=()=>{};
 globalThis.Image=class {naturalWidth=256;naturalHeight=256;set src(_){queueMicrotask(()=>this.onload?.());}};
 globalThis.document={createElement:()=>({getContext:()=>({drawImage(){}}),toDataURL:()=> 'data:image/jpeg;base64,AQID'})};
 try{await inspectReference(file);assert.notEqual(owned,file);assert.deepEqual(new Uint8Array(await owned.arrayBuffer()),new Uint8Array([1,2,3]));
 await assert.rejects(inspectReference({size:5,arrayBuffer:async()=>new ArrayBuffer(0)}),/缓存读取不完整/);
 }finally{globalThis.Image=old.Image;globalThis.document=old.document;URL.createObjectURL=old.create;URL.revokeObjectURL=old.revoke;}
});

test('human references become the same cartoon person, never an animal; own hands are preserved throughout',async()=>{
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT,/主体可以是真人、人物/);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT,/禁止把人物替换成猫狗/);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT,/同一个人物转成清晰的卡通动画角色/);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT,/人物主体自己的真人手、手指、手臂、袖口和腿脚必须保留/);
  for(const f of CUSTOM_GENERATION_FAMILIES)assert.match(f.prop_policy,/Preserve the subject's own hands/);
  const old=globalThis.fetch;const requests=[];
  globalThis.fetch=async(_url,init)=>{requests.push(JSON.parse(init.body));return Response.json({output_text:'{"pass":true,"issues":[]}'});};
  try{
    for(const kind of ['reference','video'])await reviewVisual({thinking:{apiKey:'fixture',model:'doubao-seed-2-1-lite-260915'},images:['data:image/png;base64,AA==','data:image/png;base64,AQ=='],kind});
    assert.equal(requests.length,2);
    for(const body of requests){
      assert.equal(body.text.format.type,'json_schema');assert.equal(body.text.format.strict,true);
      assert.equal(body.text.format.schema.additionalProperties,false);
      const content=body.input[0].content;
      assert.equal(content.filter(c=>c.type==='input_image').length,2);
      const prompt=content.filter(c=>c.type==='input_text').map(c=>c.text).join('\n');
      assert.match(prompt,/人物/);assert.match(prompt,/卡通/);assert.match(prompt,/纯黑背景/);
      assert.match(prompt,/第二角色/);assert.match(prompt,/手臂、手指/);
      assert.doesNotMatch(prompt,/同一只完整宠物|不能有真人手/);
      assert.doesNotMatch(prompt,/true或false/);
      assert.match(prompt,/已有随身物件/);assert.match(prompt,/球拍/);
      if(body===requests[1])assert.match(prompt,/首尾姿势可以不同/);
    }
    assert.equal(requests[0].input[0].content[0].text,'图 A：用户原始参考图。');
    assert.equal(requests[0].input[0].content[2].text,'图 B：由图 A 处理得到的角色图。');
    const referencePrompt=requests[0].input[0].content.filter(c=>c.type==='input_text').map(c=>c.text).join('\n');
    assert.match(referencePrompt,/不要求原照片肤色或写实人体比例/);
    assert.match(referencePrompt,/约 2.5 到 3 头身/);
    assert.doesNotMatch(referencePrompt,/服装配色和人体比例/);
    assert.match(BLACK_BACKGROUND_EDIT_PROMPT,/白皙.*白皮肤/);
    assert.match(BLACK_BACKGROUND_EDIT_PROMPT,/圆润大头、明亮大眼/);
    assert.doesNotMatch(BLACK_BACKGROUND_EDIT_PROMPT,/发型、肤色、服装配色和人体比例|不改变可见的脸和身体/);
    assert.match(BLACK_BACKGROUND_EDIT_PROMPT,/运动员手中的球拍/);
    for(const f of CUSTOM_GENERATION_FAMILIES){assert.match(f.prop_policy,/Preserve original character-associated accessories/);assert.match(f.prop_policy,/tennis racket/);}
  }finally{globalThis.fetch=old;}
});

test('an explicitly resumed older review rejection is rechecked once, with old decision retained',async()=>{
 const old=globalThis.fetch;let calls=0;globalThis.fetch=async()=>{calls++;return Response.json({output_text:'{"pass":true,"issues":[]}'});};
 const rejected={pass:false,issues:['旧版首尾判断']};const owner={quality:rejected,qualityResponse:{output_text:JSON.stringify(rejected)},qualityIntent:true,videoBytes:new Uint8Array([9])};
 const opts={owner,key:'quality',commit:async()=>{},thinking:{apiKey:'fixture'},images:['data:image/png;base64,AA=='],kind:'trial'};
 try{
   assert.equal(await checkpointedReview(opts),rejected);assert.equal(calls,0);
   assert.equal((await checkpointedReview({...opts,retryReview:true})).pass,true);assert.equal(calls,1);
   assert.deepEqual(owner.qualityPrevious.verdict,rejected);assert.equal(owner.qualityVersion,QUALITY_REVIEW_VERSION);assert.equal(owner.videoBytes[0],9);
   await checkpointedReview({...opts,retryReview:true});assert.equal(calls,1);
 }finally{globalThis.fetch=old;}
});

test('JSON parser rejects malformed/truncated/refusal output without leaking raw content; ignores reasoning',()=>{
  for(const raw of [{output_text:'{pass:true,issues:[]}'},{output_text:'{"pass":true,"issues":[]}',status:'incomplete'},
    {output_text:'null'},{output_text:'{"pass":true,"issues":[],}'},{output_text:'private-invalid-text'},
    {output:[{type:'message',content:[{type:'refusal',refusal:'no'}]}]}]){
    assert.throws(()=>parseQualityVerdict(raw),e=>e.code==='QUALITY_OUTPUT_INVALID'&&!e.message.includes('private-invalid-text'));
  }
  assert.deepEqual(parseQualityVerdict({output:[{type:'reasoning',content:[{type:'output_text',text:'not JSON'}]},
    {type:'message',content:[{type:'output_text',text:' \n```JSON\n{"pass":true,"issues":[]}\n```\n'}]}]}),{pass:true,issues:[]});
});

test('review checkpoint persists invalid output, explicit recovery reviews once, restart never repeats paid generation',async()=>{
  const old=globalThis.fetch;let calls=0;const saved=[];const owner={qualityIntent:true,videoBytes:new Uint8Array([9])};
  const options={owner,key:'quality',commit:async()=>saved.push(structuredClone(owner)),thinking:{apiKey:'fixture'},images:['data:image/png;base64,AA=='],kind:'trial'};
  globalThis.fetch=async()=>{calls++;return Response.json({output_text:calls===1?'{pass: true}':'{"pass":true,"issues":[]}'},{headers:{'x-request-id':'req-quality-fixture'}});};
  try{
    await assert.rejects(checkpointedReview(options),/单独补做/);assert.equal(calls,0);
    await assert.rejects(checkpointedReview({...options,retryReview:true}),e=>e.code==='QUALITY_OUTPUT_INVALID');
    assert.equal(calls,1);assert.equal(saved.at(-1).qualityResponse.output_text,'{pass: true}');
    await assert.rejects(checkpointedReview(options));assert.equal(calls,1);
    const row=getGenerationDiagnostics().find(d=>d.outcome==='invalid_output');
    assert.equal(row.stage,'trial_review');assert.equal(row.requestId,'req-quality-fixture');assert.match(formatDiagnostic(row),/解析检查结论/);
    assert.deepEqual(await checkpointedReview({...options,retryReview:true}),{pass:true,issues:[]});assert.equal(calls,2);
    await checkpointedReview({...options,retryReview:true});assert.equal(calls,2);assert.equal(owner.videoBytes[0],9);
    delete owner.quality;
    await checkpointedReview(options);assert.equal(calls,2); // saved valid response reparse, no network
  }finally{globalThis.fetch=old;}
});

test('checkpoint failure and rejected verdict never trigger another paid review',async()=>{
 const old=globalThis.fetch;let calls=0;globalThis.fetch=async()=>{calls++;return Response.json({output_text:'{"pass":true,"issues":[]}'});};
 const opts={key:'quality',thinking:{apiKey:'fixture'},images:['data:image/png;base64,AA=='],kind:'trial',retryReview:true};
 try{
  await assert.rejects(checkpointedReview({...opts,owner:{},commit:async()=>{throw Error('disk');}}),/disk/);assert.equal(calls,0);
  const rejected={pass:false,issues:['第二角色']};assert.equal(await checkpointedReview({...opts,owner:{quality:rejected,qualityVersion:QUALITY_REVIEW_VERSION},commit:async()=>{}}),rejected);assert.equal(calls,0);
 }finally{globalThis.fetch=old;}
});

test('visual verdict fails closed on malformed or absent judgments',()=>{
  assert.equal(parseQualityVerdict({output_text:'{"pass":true,"issues":["手仍在画面中"]}'}).pass,false);
  assert.deepEqual(parseQualityVerdict({output_text:'{"pass":false,"issues":["手仍在画面中"]}'}),{pass:false,issues:['手仍在画面中']});
  for(const text of ['{}','{"pass":"true","issues":[]}','{"pass":true,"issues":[2]}'])assert.throws(()=>parseQualityVerdict({output_text:text}));
});
test('visual review sends a single billed request, no automatic POST replay',async()=>{
  const old=globalThis.fetch;let count=0;
  globalThis.fetch=async()=>{count++;return new Response('temporarily unavailable',{status:503});};
  try{await assert.rejects(reviewVisual({thinking:{apiKey:'fixture',model:'doubao-seed-2-1-lite-260915'},images:['data:image/png;base64,AA=='],kind:'reference'}));assert.equal(count,1);}
  finally{globalThis.fetch=old;}
});
test('automatic resume requires original automatic consent and never repeats unknown paid submissions',()=>{
  const base={automatic:true,actions:{welcome:{taskId:'known',intent:true,deferred:true}},phase:'waiting_network'};
  assert.equal(canAutoResume(base),true);
  for(const patch of [{automatic:false},{pausedByUser:true},{finished:true},{dismissed:true},{autoResumes:3},{phase:'failed'},
    {imageIntent:true},{thinkingIntent:true},{referenceQualityIntent:true},{referenceQuality:{pass:false}},
    {actions:{welcome:{intent:true}}},{actions:{welcome:{qualityIntent:true,videoBytes:new Uint8Array([1])}}}])assert.equal(canAutoResume({...base,...patch}),false);
  assert.equal(isRecoverableQueryError(Error('HTTP 503')),true);assert.equal(isRecoverableQueryError(Error('HTTP 404')),false);
});
test('one-click UI defaults to automatic cleaning, with review only in advanced options',()=>{
  const source=readFileSync(new URL('../../CustomAvatarWizard.jsx',import.meta.url),'utf8');
  assert.match(source,/\[manualReview, setManualReview\] = useState\(false\)/);
  assert.match(source,/onNext=\{handleStartGenerate\}/);assert.match(source,/一键生成整套 · 11 段/);
  const task=readFileSync(new URL('../generation-task.js',import.meta.url),'utf8');
  assert.match(task,/confirmReference: session.automatic \? undefined/);assert.match(task,/confirmTrial: session.automatic \? undefined/);
  assert.equal(CUSTOM_GENERATION_FAMILIES.length,11);
  for(const f of CUSTOM_GENERATION_FAMILIES)assert.match(f.prop_policy,/No newly invented props/);
});

// All video inspection fixtures are local DOM/media mocks, with no provider calls.
async function videoFixture(options, run) {
 const old={document:globalThis.document,create:URL.createObjectURL,revoke:URL.revokeObjectURL,
   raf:globalThis.requestAnimationFrame,caf:globalThis.cancelAnimationFrame,setTimeout:globalThis.setTimeout,clearTimeout:globalThis.clearTimeout};
 const trace=[],timers=new Set(),stats={attached:0,removed:0,revoked:0,draws:0,raf:0,cancelled:0};let blob;
 globalThis.setTimeout=(fn,ms,...args)=>{if(ms===20000)stats.operationTimers=(stats.operationTimers||0)+1;let id;id=old.setTimeout(()=>{timers.delete(id);fn(...args);},ms===20000?10:ms);timers.add(id);return id;};
 globalThis.clearTimeout=id=>{timers.delete(id);return old.clearTimeout(id);};
 class Video extends EventTarget {
  style={};duration=options.duration??5;videoWidth=options.width??256;videoHeight=options.height??256;
  callbacks=new Map();next=0;src='';_time=0;seeking=false;
  setAttribute(){}removeAttribute(){this.src='';}pause(){stats.paused=true;}
  remove(){stats.removed++;stats.attached--;}
  requestVideoFrameCallback(cb){trace.push('request');const id=++this.next;this.callbacks.set(id,cb);return id;}
  cancelVideoFrameCallback(id){stats.cancelled++;this.callbacks.delete(id);}
  present(){trace.push('present');const callbacks=[...this.callbacks.values()];this.callbacks.clear();for(const cb of callbacks)cb(0,{mediaTime:this._time});}
  transition(event,missing=false){queueMicrotask(()=>{
   if(options.frameFirst){if(!missing)this.present();this.dispatchEvent(new Event(event));}
   else{this.dispatchEvent(new Event(event));if(!missing)this.present();}
  });}
  load(){if(this.src){trace.push('load');if(options.loadError)queueMicrotask(()=>this.dispatchEvent(new Event('error')));else this.transition('loadeddata');}}
  set currentTime(value){
   this._time=value;trace.push('seek');options.onSeek?.();
   if(options.staleWhileSeeking){
    this.seeking=true;
    queueMicrotask(()=>{
     this.present(); // stale callback must re-register, not count as the frame
     assert.equal(this.callbacks.size,1);trace.push('stale-rearmed');
     options.afterStale?.();this.seeking=false;
     if(options.freshBeforeSeeked){if(!options.missingFresh)this.present();this.dispatchEvent(new Event('seeked'));}
     else{this.dispatchEvent(new Event('seeked'));if(!options.missingFresh)this.present();}
    });
   }else this.transition('seeked',options.missingFrame);
  }
  get currentTime(){return this._time;}
 }
 const video=new Video();if(options.noRvfc){video.requestVideoFrameCallback=undefined;video.cancelVideoFrameCallback=undefined;}
 const canvas={width:0,height:0,getContext:()=>({drawImage(){stats.draws++;assert.equal(video.callbacks.size,0);assert.equal(stats.attached,1);},
   getImageData(){if(options.canvasError)throw Error('fixture private URL must not escape');return {data:new Uint8ClampedArray(options.black?[0,0,0,255]:[21,0,0,255])};}}),toDataURL:()=> 'data:image/jpeg;base64,AQID'};
 globalThis.document={body:{appendChild(node){assert.equal(node,video);stats.attached++;}},createElement:kind=>kind==='video'?video:canvas};
 URL.createObjectURL=value=>{blob=value;return 'blob:local-fixture';};URL.revokeObjectURL=()=>{stats.revoked++;};
 globalThis.requestAnimationFrame=cb=>{stats.raf++;queueMicrotask(cb);return stats.raf;};globalThis.cancelAnimationFrame=()=>{};
 try{return await run({video,stats,trace,timers,blob:()=>blob});}
 finally{globalThis.document=old.document;URL.createObjectURL=old.create;URL.revokeObjectURL=old.revoke;
  globalThis.requestAnimationFrame=old.raf;globalThis.cancelAnimationFrame=old.caf;globalThis.setTimeout=old.setTimeout;globalThis.clearTimeout=old.clearTimeout;
  for(const id of timers)old.clearTimeout(id);
 }
}
for(const frameFirst of [false,true])test(`video waits for both seeked and presented frame (frameFirst=${frameFirst})`,async()=>{
 await videoFixture({frameFirst},async({video,stats,trace,timers})=>{
  const result=await inspectVideo(new Uint8Array([1,2,3]));assert.equal(result.frames.length,3);
  assert.equal(stats.draws,3);assert.equal(stats.raf,0);assert.equal(video.muted,true);assert.equal(video.defaultMuted,true);
  assert.notEqual(video.style.display,'none');assert.equal(video.style.left,'-10000px');
  assert.equal(trace[0],'request');assert.equal(trace[1],'load');
  for(let at=0;at<trace.length;at++)if(trace[at]==='seek')assert.equal(trace[at-1],'request');
  assert.equal(stats.removed,1);assert.equal(stats.attached,0);assert.equal(stats.revoked,1);assert.equal(timers.size,0);
 });
});
test('advertised rVFC missing after seek fails bounded and never falls back to RAF',async()=>{
 await videoFixture({missingFrame:true},async({video,stats,timers})=>{
  await assert.rejects(inspectVideo(new Uint8Array([1])),error=>{
   assert.deepEqual(videoInspectionFailure(error),{code:'VIDEO_INSPECTION_FRAME_TIMEOUT',stage:'frame'});return true;
  });assert.equal(stats.draws,0);assert.equal(stats.raf,0);assert.equal(video.callbacks.size,0);
  assert.equal(stats.removed,1);assert.equal(stats.revoked,1);assert.equal(timers.size,0);
 });
});
test('video cancellation clears presentation callbacks, timers and DOM without sampling',async()=>{
 const controller=new AbortController();await videoFixture({onSeek:()=>controller.abort()},async({video,stats,timers})=>{
  await assert.rejects(inspectVideo(new Uint8Array([1]),controller.signal),error=>{
   assert.equal(error.name,'AbortError');assert.equal(error.code,'VIDEO_INSPECTION_ABORTED');assert.equal(videoInspectionFailure(error),null);return true;
  });assert.equal(video.callbacks.size,0);assert.equal(stats.draws,0);assert.equal(timers.size,0);assert.equal(stats.removed,1);assert.equal(stats.revoked,1);
 });
});
test('video Blob owns exactly a byte-offset view, not adjacent backing bytes',async()=>{
 await videoFixture({},async({blob})=>{
  const backing=new Uint8Array([99,1,2,3,88]);const view=backing.subarray(1,4);
  const pending=inspectVideo(view);backing.fill(0);await pending;
  assert.deepEqual([...new Uint8Array(await blob().arrayBuffer())],[1,2,3]);
 });
});
test('true black video remains rejected and cleaned after presented-frame proof',async()=>{
 await videoFixture({black:true},async({stats})=>{
  await assert.rejects(inspectVideo(new Uint8Array([1])),error=>error.code==='VIDEO_INSPECTION_BLACK_FRAME'&&error.inspectionStage==='validate');
  assert.equal(stats.draws,1);assert.equal(stats.removed,1);assert.equal(stats.revoked,1);
 });
});
test('browsers without rVFC retain two-RAF compatibility for each sampled frame',async()=>{
 await videoFixture({noRvfc:true},async({stats})=>{await inspectVideo(new Uint8Array([1]));assert.equal(stats.raf,6);assert.equal(stats.draws,3);});
});
test('video local decode/canvas failures are typed without exposing raw diagnostic strings',async()=>{
 for(const options of [{loadError:true},{canvasError:true}])await videoFixture(options,async({stats})=>{
  await assert.rejects(inspectVideo(new Uint8Array([1])),error=>{
   assert.match(error.code,/^VIDEO_INSPECTION_/);assert.doesNotMatch(error.message,/private URL/);return true;
  });assert.equal(stats.removed,1);assert.equal(stats.revoked,1);
 });
 assert.deepEqual(videoInspectionFailure(Error('raw private URL')),{stage:'inspection',code:'VIDEO_INSPECTION_LOCAL_ERROR'});
});
test('video size duration geometry and visibility thresholds are not bypassed',async()=>{
 await assert.rejects(inspectVideo(new Uint8Array(0)),error=>error.code==='VIDEO_INSPECTION_INPUT_INVALID');
 await assert.rejects(inspectVideo({byteLength:100*1024*1024+1}),error=>error.code==='VIDEO_INSPECTION_INPUT_INVALID');
 for(const options of [{duration:0.99},{duration:30.01},{width:127},{height:127},{width:128,height:1024}])await videoFixture(options,async()=>{
  await assert.rejects(inspectVideo(new Uint8Array([1])),error=>error.inspectionStage==='validate');
 });
});

for(const freshBeforeSeeked of [false,true])test(`stale seeking callback waits for fresh frame (freshBeforeSeeked=${freshBeforeSeeked})`,async()=>{
 await videoFixture({staleWhileSeeking:true,freshBeforeSeeked},async({stats,trace,video,timers})=>{
  const result=await inspectVideo(new Uint8Array([1]));assert.equal(result.frames.length,3);
  assert.equal(trace.filter(t=>t==='seek').length,3);assert.equal(trace.filter(t=>t==='stale-rearmed').length,3);assert.equal(stats.operationTimers,4);
  assert.equal(stats.draws,3);assert.equal(video.callbacks.size,0);assert.equal(timers.size,0);
  assert.equal(stats.removed,1);assert.equal(stats.revoked,1);
 });
});
test('stale seeking callback without a fresh presentation times out on the original operation',async()=>{
 await videoFixture({staleWhileSeeking:true,missingFresh:true},async({stats,video,trace,timers})=>{
  await assert.rejects(inspectVideo(new Uint8Array([1])),error=>error.code==='VIDEO_INSPECTION_FRAME_TIMEOUT'&&error.inspectionStage==='frame');
  assert.equal(trace.filter(t=>t==='seek').length,1);assert.equal(stats.draws,0);assert.equal(stats.operationTimers,2);
  assert.equal(video.callbacks.size,0);assert.equal(timers.size,0);assert.equal(stats.removed,1);
 });
});
test('abort after stale presentation cancels the latest re-registered callback',async()=>{
 const controller=new AbortController();await videoFixture({staleWhileSeeking:true,missingFresh:true,afterStale:()=>controller.abort()},async({video,stats,timers})=>{
  await assert.rejects(inspectVideo(new Uint8Array([1]),controller.signal),error=>error.name==='AbortError'&&videoInspectionFailure(error)===null);
  assert.equal(video.callbacks.size,0);assert.equal(stats.draws,0);assert.equal(timers.size,0);
  assert.equal(stats.removed,1);assert.equal(stats.revoked,1);
 });
});
