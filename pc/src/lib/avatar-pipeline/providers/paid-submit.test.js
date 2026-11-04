import test from 'node:test';
import assert from 'node:assert/strict';
import {runKlingFamily} from './kling.js';
import {runCustomFamily} from './custom.js';
import {runAvatarPipeline} from '../run.js';
import {canAutoResume,assertResumableAction} from '../session.js';
const configs={kling:{provider:'kling',accessKey:'synthetic-access',secretKey:'synthetic-secret',baseUrl:'https://fixture.invalid',model:'fixture'},custom:{provider:'custom',apiKey:'synthetic-key',baseUrl:'https://fixture.invalid',model:'fixture'}};
const execute=(provider)=>provider==='kling'?runKlingFamily({config:configs.kling,prompt:'fixture',imageBase64:'AA=='}):runCustomFamily({config:configs.custom,prompt:'fixture',imageDataUrl:'data:image/png;base64,AA=='});
async function isolated(fetcher,fn){const oldFetch=globalThis.fetch,oldTimeout=globalThis.setTimeout;globalThis.fetch=fetcher;globalThis.setTimeout=(f,ms,...args)=>oldTimeout(f,ms>=2000?0:ms,...args);try{return await fn();}finally{globalThis.fetch=oldFetch;globalThis.setTimeout=oldTimeout;}}
for(const provider of Object.keys(configs)){
 for(const status of [408,429,500,502,503,504])test(`${provider} create HTTP ${status} sends one POST only`,async()=>{
  let posts=0;await isolated(async(_url,init)=>{assert.equal(init.method,'POST');posts++;return new Response('{"error":"synthetic ambiguous response"}',{status});},async()=>await assert.rejects(execute(provider),new RegExp(`HTTP ${status}`)));assert.equal(posts,1);
 });
 test(`${provider} lost create response sends one POST only`,async()=>{let posts=0;await isolated(async(_url,init)=>{assert.equal(init.method,'POST');posts++;throw Error('synthetic transport failure');},async()=>assert.rejects(execute(provider)));assert.equal(posts,1);});
 test(`${provider} GET polling and download retain retries without another create`,async()=>{
  let posts=0,queries=0,downloads=0;
  await isolated(async(url,init={})=>{
   if(init.method==='POST'){posts++;return Response.json({data:{task_id:'synthetic-task'}});}
   if(String(url)==='https://cdn.invalid/synthetic.mp4'){downloads++;if(downloads===1)throw Error('synthetic read-only download interruption');return new Response(new Uint8Array([1,2,3]));}
   queries++;if(queries===1)return new Response('retry read-only',{status:503});
   return Response.json(provider==='kling'?{data:{task_status:'succeed',task_result:{videos:[{url:'https://cdn.invalid/synthetic.mp4'}]}}}:{status:'succeeded',data:[{url:'https://cdn.invalid/synthetic.mp4'}]});
  },async()=>{const result=await execute(provider);assert.deepEqual(result.videoBytes,new Uint8Array([1,2,3]));});
  assert.deepEqual({posts,queries,downloads},{posts:1,queries:2,downloads:2});
 });
 test(`${provider} runner retains unknown intent; same-session resume creates no new job`,async()=>{
  const session={automatic:true,actions:{},referenceApproved:true,manifest:{persona:{},entries:[{family:'welcome',prompt:'fixture movement'}]}};let posts=0;const commits=[];
  const args={imageFile:new File(['synthetic'],'fixture.png',{type:'image/png'}),providerConfig:configs[provider],skipProcessing:true,familyIds:['welcome'],session,checkpoint:async s=>commits.push(structuredClone(s))};
  await isolated(async(_url,init={})=>{assert.equal(init.method,'POST');posts++;return new Response('synthetic lost-result gateway',{status:504});},async()=>{
   const first=await runAvatarPipeline(args);assert.equal(first.families[0].ok,false);assert.equal(posts,1);assert.equal(session.actions.welcome.intent,true);assert.equal(session.actions.welcome.taskId,undefined);
   assert.equal(canAutoResume(session),false);assert.throws(()=>assertResumableAction(session.actions.welcome,provider),/不会自动/);
   const second=await runAvatarPipeline(args);assert.equal(second.families[0].ok,false);assert.equal(posts,1);assert.equal(session.actions.welcome.intent,true);
   assert.ok(commits.some(c=>c.actions.welcome?.intent));
  });
 });
}
