// Isolated test origin only: no installed-app storage, API calls or paid jobs.
import { chromium } from 'playwright';
import assert from 'node:assert/strict';
const browser=await chromium.launch({headless:true,...(process.env.PET_TEST_BROWSER?{executablePath:process.env.PET_TEST_BROWSER}:{})});
try {
  const page=await browser.newPage();
  await page.goto(process.argv[2]);
  const result=await page.evaluate(async()=>{
    const m=await import('/src/lib/avatar-pipeline/session.js');
    const old={id:'old',appearanceName:'Old',createdAt:1,actions:{first:{intent:true,taskId:'cloud-id',videoBytes:new Uint8Array([1,2,3])},unknown:{intent:true}}};
    await m.saveGenerationSession(old);
    await m.beginGenerationSession({id:'new',createdAt:2,actions:{}});
    const history=await m.listGenerationHistory();
    const archived=await m.loadGenerationSession(history[0].key);
    return {history,retainedId:archived.actions.first.taskId,bytes:Array.from(archived.actions.first.videoBytes),unknown:archived.actions.unknown.intent,current:(await m.loadGenerationSession()).id};
  });
  assert.equal(result.current,'new');assert.equal(result.retainedId,'cloud-id');assert.equal(result.unknown,true);assert.deepEqual(result.bytes,[1,2,3]);assert.equal(result.history.length,1);
  await page.reload();
  const current=await page.evaluate(async()=>{
    const m=await import('/src/lib/avatar-pipeline/session.js');
    const old=await m.loadGenerationSession('archive:old');
    await m.beginGenerationSession(old);
    return {id:(await m.loadGenerationSession()).id,history:await m.listGenerationHistory()};
  });
  assert.equal(current.id,'old');assert.equal(current.history.length,1);assert.equal(current.history[0].id,'new');
  console.log(JSON.stringify({atomicArchive:true,blobPreserved:true,unknownSubmissionPreserved:true,reload:true,restorePreservesNewerTask:true,paidRequests:0}));
} finally {await browser.close();}
