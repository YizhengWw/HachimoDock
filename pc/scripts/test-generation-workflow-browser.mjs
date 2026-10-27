import {chromium} from 'playwright';
import fs from 'node:fs';
import assert from 'node:assert/strict';
const [url,dir]=process.argv.slice(2);
const browser=await chromium.launch({headless:true,...(process.env.PET_TEST_BROWSER ? {executablePath:process.env.PET_TEST_BROWSER} : {})});
try {
  const page=await browser.newPage({viewport:{width:1100,height:1050}});
  const errors=[];page.on('pageerror',error=>errors.push(error.message));
  await page.goto(url);await page.waitForFunction(()=>typeof window.showWorkflowPreview==='function');
  await page.evaluate(data=>window.showWorkflowPreview('reference',data),'data:image/png;base64,'+fs.readFileSync(dir+'/processed.png').toString('base64'));
  await page.getByRole('img',{name:'处理后的宠物参考图'}).waitFor();
  await page.screenshot({path:dir+'/reference-confirmation.png',fullPage:true});
  await page.getByRole('button',{name:'参考图正确，生成一段试片'}).click();
  assert.equal(await page.locator('#stage').textContent(),'trial');
  await page.getByRole('button',{name:'试片满意，生成剩余动作'}).click();
  assert.equal(await page.locator('#stage').textContent(),'done');
  // Exercise the real IDB transaction across a page reload; never touches app-native storage.
  await page.evaluate(async()=>{
    const {saveGenerationSession,loadGenerationSession}=await import('/src/lib/avatar-pipeline/session.js');
    await saveGenerationSession({id:'offline-test',actions:{done:{taskId:'known-id',videoBytes:new Uint8Array([1,2,3])}},reference:{bytes:new Uint8Array([4,5])}});
    if((await loadGenerationSession()).actions.done.videoBytes[2]!==3)throw Error('IDB bytes failed');
  });
  await page.reload();
  const recovered=await page.evaluate(async()=>{
    const {loadGenerationSession}=await import('/src/lib/avatar-pipeline/session.js');return (await loadGenerationSession()).actions.done.taskId;
  });
  assert.equal(recovered,'known-id');assert.deepEqual(errors,[]);
  console.log(JSON.stringify({referenceGate:true,trialGate:true,idbReload:true,pageErrors:errors}));
} finally {await browser.close();}
