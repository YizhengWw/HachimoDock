import test from 'node:test';
import assert from 'node:assert/strict';
import { serialQueue, publicGenerationConfig, assertResumableAction, generationSessionNeedsProtection, summarizeGenerationSession } from './session.js';

test('durable summary separates review, partial, stopped and actual failure states',()=>{
  const session={manifest:{entries:[{family:'welcome'},{family:'done'}]},actions:{welcome:{videoBytes:new Uint8Array([1])}}};
  let s=summarizeGenerationSession(session);
  assert.equal(s.completed,1);assert.equal(s.total,2);assert.equal(s.status,'paused');assert.match(s.label,/试片/);
  s=summarizeGenerationSession({...session,trialApproved:true});assert.match(s.label,/部分/);
  s=summarizeGenerationSession({...session,phase:'failed'});assert.equal(s.status,'failed');assert.match(s.label,/部分已保存/);
  s=summarizeGenerationSession({...session,dismissed:true});assert.match(s.label,/已停止/);
  s=summarizeGenerationSession({reference:{},actions:{}});assert.match(s.label,/参考图/);assert.equal(s.total,11);
  s=summarizeGenerationSession({...session,finished:true,actions:{...session.actions,done:{videoBytes:new Uint8Array([2])}}});assert.equal(s.status,'completed');
  assert.equal(summarizeGenerationSession({actions:{welcome:{videoBytes:new Uint8Array()}}}).completed,0);
});

test('paid reference results, unknown videos and partial results stay recoverable; new jobs archive independently',()=>{
  for(const session of [null,{}, {imageIntent:true,actions:{}}, {dismissed:true,actions:{a:{intent:true}}}, {finished:true}])
    assert.equal(generationSessionNeedsProtection(session),false);
  for(const action of [{intent:true},{taskId:'id'},{videoBytes:new Uint8Array([1])},{videoUrl:'https://example.test/v'}])
    assert.equal(generationSessionNeedsProtection({actions:{a:action}}),true);
  assert.equal(generationSessionNeedsProtection({appearanceId:'saved'}),true);
  assert.equal(generationSessionNeedsProtection({reference:{bytes:new Uint8Array([1])},manifest:{}}),true);
});
import { runAvatarPipeline } from './run.js';
import { buildVolcanoTaskPayload } from './providers/volcano.js';
import { processImageForPipeline } from './image-processing.js';

test('journal config omits all credentials and URL overrides', () => {
  assert.deepEqual(publicGenerationConfig({provider:'volcengine',model:'test',apiKey:'secret',accessKey:'secret',secretKey:'secret',baseUrl:'https://user:password@host',imageEdit:{apiKey:'secret'},advanced:{authHeader:'secret'}}),{provider:'volcengine',model:'test'});
});

test('serial saves never overlap and a failed save does not poison the queue', async () => {
  const queue=serialQueue(); let active=0; let maximum=0; const order=[];
  const jobs=Array.from({length:8},(_,i)=>queue(async()=>{
    maximum=Math.max(maximum,++active); await new Promise(r=>setTimeout(r,1));
    order.push(i); active--; if(i===2) throw Error('disk');
  }));
  await Promise.allSettled(jobs); assert.equal(maximum,1); assert.deepEqual(order,[0,1,2,3,4,5,6,7]);
});

test('unknown paid submissions are never replayed; known Ark task IDs are resumable', () => {
  assert.throws(()=>assertResumableAction({intent:true},'volcengine'),/不会自动/);
  assert.throws(()=>assertResumableAction({intent:true,taskId:'task'},'custom'),/不会自动/);
  assert.doesNotThrow(()=>assertResumableAction({intent:true,taskId:'task'},'volcengine'));
  assert.doesNotThrow(()=>assertResumableAction({intent:true,videoBytes:new Uint8Array([1])},'custom'));
});

test('missing background service blocks before canvas or downstream generation', async()=>{
  await assert.rejects(processImageForPipeline(new Blob(['image'])),/尚未生成视频/);
});

test('Seedance 2.5 uses adaptive frame ratio; current Mini keeps 4:3',()=>{
  for(const [model,ratio] of [['doubao-seedance-2-5-pro-test','adaptive'],['doubao-seedance-2-0-mini-260615','4:3']]) {
    const body=buildVolcanoTaskPayload({model,ratio:'4:3',imageDataUrl:'data:image/png;base64,AA==',prompt:'move'});
    assert.equal(body.ratio,ratio);
    assert.equal(body.camera_fixed, undefined);
  }
});

test('reference gate precedes every paid request; trial gate allows exactly one video; recovery reuses all results', async()=>{
  const priorFetch=globalThis.fetch;
  let prompts=0, submissions=0, queries=0, activeSaves=0,maxSaves=0;
  const ids=['welcome','done','working'];
  const session={actions:{}}; const commits=[];
  const mockFetch=async(url,init)=>{
    if(String(url).endsWith('/responses')) {prompts++;return Response.json({output_text:JSON.stringify({persona:{},prompts:ids.map(family=>({family,prompt:'safe movement'}))})});}
    if(init.method==='POST') { submissions++; return Response.json({id:`cloud-${submissions}`}); }
    if(String(url).includes('/tasks/')) {queries++;return Response.json({status:'succeeded',content:{video_url:'https://trial.invalid/video'}});}
    return new Response(new Uint8Array([1,2,3]));
  };
  globalThis.fetch=mockFetch;
  const args={imageFile:new File(['bytes'],'test.png',{type:'image/png'}),skipProcessing:true,
    providerConfig:{provider:'volcengine',apiKey:'fake',model:'doubao-seedance-2-0-mini-test'},familyIds:ids,session,
    checkpoint:async s=>{commits.push(structuredClone(s));},
    confirmReference:async()=>{assert.equal(prompts,0);assert.equal(submissions,0);},
    confirmTrial:async(f,remaining)=>{assert.equal(submissions,1);assert.equal(remaining,2);assert.equal(f.ok,true);},
    onFamilyDone:async()=>{maxSaves=Math.max(maxSaves,++activeSaves);await new Promise(r=>setTimeout(r,3));activeSaves--;},
  };
  try {
    const first=await runAvatarPipeline(args);
    assert.ok(commits.some(s=>s.phase==='reference_review'&&!s.referenceApproved));
    assert.ok(commits.some(s=>s.phase==='trial_review'&&s.actions.welcome.videoBytes&&!s.trialApproved));
    assert.equal(first.families.filter(f=>f.ok).length,3);assert.equal(submissions,3);assert.equal(prompts,1);assert.equal(maxSaves,1);
    assert.ok(commits.some(s=>s.actions.welcome?.taskId && !s.actions.welcome.videoBytes));
    const before={prompts,submissions,queries};
    await runAvatarPipeline({...args,confirmReference:()=>assert.fail('reference reapproved'),confirmTrial:()=>assert.fail('trial reapproved')});
    assert.deepEqual({prompts,submissions,queries},before);
    // Simulate restart after an accepted cloud submission, before download.
    delete session.actions.done.videoBytes;
    await runAvatarPipeline(args);assert.equal(submissions,3);assert.equal(queries,before.queries+1);
  } finally {globalThis.fetch=priorFetch;}
});

test('checkpoint failure stops all subsequent paid work',async()=>{
  const priorFetch=globalThis.fetch;let requests=0;
  globalThis.fetch=async()=>{requests++;throw Error('unexpected');};
  try {
    await assert.rejects(runAvatarPipeline({imageFile:new File(['x'],'x.png'),skipProcessing:true,
      providerConfig:{provider:'volcengine',apiKey:'fake',model:'test'},session:{},checkpoint:async()=>{throw Error('disk full');}}),/disk full/);
    assert.equal(requests,0);
  } finally {globalThis.fetch=priorFetch;}
});

test('local save failure preserves generated bytes and does not double-count progress',async()=>{
  const session={actions:{}};const progress=[];
  await assert.rejects(runAvatarPipeline({imageFile:new File(['x'],'x.png',{type:'image/png'}),
    providerConfig:{provider:'volcengine',model:'test'},skipProcessing:true,dryRun:true,
    familyIds:['welcome'],session,checkpoint:async()=>{},
    onProgress:p=>progress.push(p),onFamilyDone:async()=>{throw Error('disk unavailable');}}),/本地保存失败/);
  assert.ok(session.actions.welcome.videoBytes.byteLength>0);
  assert.ok(progress.every(p=>p.completed<=p.total));
  assert.equal(progress.at(-1).completed,1);
  assert.equal(summarizeGenerationSession(session).completed,1);
});
