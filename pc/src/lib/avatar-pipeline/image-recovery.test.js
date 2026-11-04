import test from 'node:test';
import assert from 'node:assert/strict';
import {buildVolcanoImageEditPayload,editImageBackgroundWithArk} from './providers/volcano-image.js';
import {imageResultReceipt,recoverableImageEdit,shouldRetryImagePost,submitImageWithKnownRejectionRetries,downloadKnownImage} from './image-recovery.js';
import {ensureActionStartFrame} from './action-start-frame.js';
const receipt=()=>imageResultReceipt({url:'https://result.example/image.png?token=fixture'},'same-model');
const transient=()=>Object.assign(new Error('GET failed'),{diagnostic:{outcome:'timeout',status:200}});
const rejects=code=>Object.assign(new Error('definitive rejection'),{status:429,completeProviderResponse:true,providerCode:code});
async function withFetch(fn,run){const old=globalThis.fetch;globalThis.fetch=fn;try{return await run();}finally{globalThis.fetch=old;}}

test('URL image response keeps actual model,size,PNG and nonstream payload',()=>{
 const p=buildVolcanoImageEditPayload({model:'doubao-seedream-5-0-flash-260915',imageDataUrl:'data:image/png;base64,AQ==',prompt:'fixture'});
 assert.equal(p.response_format,'url');assert.equal(p.model,'doubao-seedream-5-0-flash-260915');assert.equal(p.size,'2K');assert.equal(p.output_format,'png');assert.equal(p.stream,undefined);
});
test('complete result checkpoint precedes GET and GET retry never regenerates',async()=>{
 let posts=0,gets=0,saved;
 await withFetch(async(url,init)=>{
  if(init.method==='POST'){posts++;return Response.json({model:'same-model',data:[{url:'https://result.example/one'}]});}
  gets++;assert.equal(saved?.url,url);if(gets<3)throw Error('temporary GET interruption');return new Response(new Uint8Array([137,80,78,71,0,0,0,0]));
 },async()=>{const r=await editImageBackgroundWithArk({apiKey:'fixture-only',imageDataUrl:'data:image/png;base64,AQ==',onResult:async r=>{saved=structuredClone(r);}});assert.equal(r.bytes.length,8);});
 assert.equal(posts,1);assert.equal(gets,3);assert.equal(saved.model,'same-model');
});
test('only five complete429 codes allow at most two POST retries',async()=>{
 for(const code of ['ModelAccountIpmRateLimitExceeded','ModelAccountConcurrentRequestsLimitExceeded','AccountRateLimitExceeded','ServerOverloaded','RequestBurstTooFast'])assert(shouldRetryImagePost(rejects(code)));
 for(const e of [rejects('QuotaExceeded'),rejects('SetLimitExceeded'),rejects('AccountQuotaExceeded'),Object.assign(rejects('ServerOverloaded'),{status:500}),Object.assign(rejects('ServerOverloaded'),{completeProviderResponse:false}),new Error('body timeout')])assert(!shouldRetryImagePost(e));
 let posts=0;await assert.rejects(submitImageWithKnownRejectionRetries(async()=>{posts++;throw rejects('ServerOverloaded');},{sleepFn:async()=>{}}));assert.equal(posts,3);
});
test('actual provider parses complete429 code before one bounded retry',async()=>{
 let posts=0,gets=0;
 await withFetch(async(_url,init)=>{
  if(init.method==='POST'){posts++;return posts===1?Response.json({error:{code:'ModelAccountIpmRateLimitExceeded'}},{status:429}):Response.json({data:[{url:'https://result.example/two'}]});}
  gets++;return new Response(new Uint8Array([1,2]));
 },()=>editImageBackgroundWithArk({apiKey:'fixture-only',imageDataUrl:'data:image/png;base64,AQ==',onResult:async()=>{}}));
 assert.equal(posts,2);assert.equal(gets,1);
});
test('saved URL resumes GET only; expired4xx no retry; failed persistence no GET',async()=>{
 let saved,posts=0,gets=0;
 await assert.rejects(recoverableImageEdit({submit:async()=>{posts++;return receipt();},onResult:async r=>{saved=r;},download:async()=>{gets++;throw Object.assign(new Error('expired403'),{status:403});}}));
 assert.equal(posts,1);assert.equal(gets,1);
 const result=await recoverableImageEdit({resumeResult:saved,submit:async()=>{throw Error('must not regenerate');},onResult:async()=>{},download:async()=>{gets++;return new Uint8Array([2]);}});
 assert.equal(result.bytes[0],2);assert.equal(posts,1);assert.equal(gets,2);
 await assert.rejects(recoverableImageEdit({submit:async()=>receipt(),onResult:async()=>{throw Error('storage failed');},download:async()=>{throw Error('GET must not happen');}}),/storage failed/);
});
test('action-start URL persists before GET and explicit recovery does not rePOST',async()=>{
 let posts=0,gets=0,persisted;const owner={startFrameQuality:{pass:true,issues:[]}};const actions={working:owner};
 const args={owner,actions,family:'working',reference:{bytes:new Uint8Array([1]),dataUrl:'data:image/png;base64,AQ=='},cloudBackground:{apiKey:'fixture-only'},thinking:{},commit:async()=>{persisted=structuredClone(owner);},normalize:async raw=>raw.bytes};
 await withFetch(async(_url,init)=>{if(init.method==='POST'){posts++;return Response.json({data:[{url:'https://result.example/action'}]});}gets++;assert(persisted.startFrameResult?.url);return new Response('expired',{status:403});},async()=>{await assert.rejects(ensureActionStartFrame(args));});
 assert.equal(posts,1);assert.equal(gets,1);assert(owner.startFrameResult);assert(!owner.startFrameRaw);
 await withFetch(async(_url,init)=>{assert.notEqual(init.method,'POST');gets++;return new Response(new Uint8Array([2]));},async()=>{const r=await ensureActionStartFrame(args);assert.equal(r.bytes[0],2);});
 assert.equal(posts,1);assert.equal(gets,2);
});
test('unknown POST outcome is never retried or converted into known URL',async()=>{
 let posts=0,saved=0;await withFetch(async()=>{posts++;throw Error('request outcome unknown');},async()=>{await assert.rejects(editImageBackgroundWithArk({apiKey:'fixture-only',imageDataUrl:'data:image/png;base64,AQ==',onResult:async()=>{saved++;}}));});assert.equal(posts,1);assert.equal(saved,0);
});
test('GET attempt budgets are short and max3; abort never starts next retry',async()=>{
 let calls=0;await assert.rejects(downloadKnownImage(receipt(),{sleepFn:async()=>{},download:async(_u,_s,o)=>{calls++;assert(o.timeoutMs<=15000);assert.equal(o.maxBytes,67108864);throw transient();}}));assert.equal(calls,3);
 const c=new AbortController();calls=0;await assert.rejects(downloadKnownImage(receipt(),{signal:c.signal,download:async()=>{calls++;c.abort();throw transient();}}),{name:'AbortError'});assert.equal(calls,1);
 await assert.rejects(downloadKnownImage(receipt(),{totalMs:20,download:(_u,signal)=>new Promise((_,reject)=>signal.addEventListener('abort',()=>reject(new DOMException('aborted','AbortError')),{once:true}))}),/超过45秒/);
});
