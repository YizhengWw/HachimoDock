// Headless isolated test origin, synthetic media, all external requests mocked.
import {chromium} from 'playwright';
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {CUSTOM_GENERATION_FAMILIES} from '../src/lib/avatar-pipeline/families.js';
const [url,dir]=process.argv.slice(2);const video=fs.readFileSync(dir+'/fixture.mp4');
const browser=await chromium.launch({headless:true,...(process.env.PET_TEST_BROWSER?{executablePath:process.env.PET_TEST_BROWSER}:{})});
const counts={images:0,prompts:0,quality:0,videos:0};let rejectReference=false;let png='';
try{
 const page=await browser.newPage({viewport:{width:1100,height:1000}});const errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.route('**/*',async route=>{
  const r=route.request(),u=r.url();if(u.startsWith(new URL(url).origin)||u.startsWith('blob:'))return route.continue();
  const json=b=>route.fulfill({status:200,contentType:'application/json',body:JSON.stringify(b)});
  if(u.endsWith('/models'))return json({data:[{id:'doubao-seedance-2-0-fast-260128'}]});
  if(u.endsWith('/images/generations')){counts.images++;return json({data:[{b64_json:png}]});}
  if(u.endsWith('/responses')){
   const b=r.postDataJSON();const text=b.input[0].content.map(c=>c.text||'').join('');
   if(text.includes('检查员')){counts.quality++;return json({output_text:JSON.stringify({pass:!(rejectReference&&text.includes('参考图检查员')),issues:rejectReference?['fixture rejected']:[]})});}
   counts.prompts++;return json({output_text:JSON.stringify({persona:{},prompts:CUSTOM_GENERATION_FAMILIES.map(f=>({family:f.family,prompt:'Same complete subject moves on pure black and returns.'}))})});
  }
  if(u.includes('/contents/generations/tasks')){
   if(r.method()==='POST'){counts.videos++;return json({id:'known-'+counts.videos});}
   return json({status:'succeeded',content:{video_url:'https://fixture.invalid/test.mp4'}});
  }
  if(u==='https://fixture.invalid/test.mp4')return route.fulfill({status:200,contentType:'video/mp4',body:video});
  return route.abort('blockedbyclient');
 });
 await page.goto(url);try{await page.getByRole('button',{name:'一键生成整套 · 11 段'}).waitFor();}catch(e){throw Error(`${e.message}; page errors: ${errors.join(';')}`);}
 png=await page.evaluate(()=>{const c=document.createElement('canvas');c.width=384;c.height=288;const x=c.getContext('2d');x.fillStyle='black';x.fillRect(0,0,384,288);x.fillStyle='orange';x.fillRect(140,80,100,150);return c.toDataURL('image/png').split(',')[1];});
 await page.locator('input[type=file]').first().setInputFiles({name:'fixture.png',mimeType:'image/png',buffer:Buffer.from(png,'base64')});
 await page.waitForFunction(()=>Array.from(document.querySelectorAll('button')).some(b=>b.textContent.includes('一键生成整套')&&!b.disabled));
 assert.equal(await page.getByRole('button',{name:'高级设置 / 模型与逐步确认'}).isVisible(),true);
 await page.screenshot({path:dir+'/one-click.png',fullPage:true});
 const first=await page.evaluate(async png=>{
  const {runAvatarPipeline}=await import('/src/lib/avatar-pipeline/run.js');
  const {saveGenerationSession,loadGenerationSession}=await import('/src/lib/avatar-pipeline/session.js');
  const imageFile=new File([Uint8Array.from(atob(png),c=>c.charCodeAt(0))],'fixture.png',{type:'image/png'});
  const session={id:'isolated-auto-test',automatic:true,actions:{},imageFile};
  const saved=[];const result=await runAvatarPipeline({imageFile,providerConfig:{provider:'volcengine',apiKey:'offline-fixture',model:'doubao-seedance-2-0-fast-260128'},automatic:true,session,checkpoint:saveGenerationSession,onFamilyDone:async f=>saved.push(f.family.family)});
  return {ok:result.families.filter(f=>f.ok).length,saved:saved.length,persisted:Object.keys((await loadGenerationSession()).actions).length};
 },png);
 assert.deepEqual(first,{ok:11,saved:11,persisted:11});assert.deepEqual(counts,{images:1,prompts:1,quality:2,videos:11});
 const before={...counts};await page.reload();
 await page.evaluate(async()=>{const {loadGenerationSession,saveGenerationSession}=await import('/src/lib/avatar-pipeline/session.js');const {runAvatarPipeline}=await import('/src/lib/avatar-pipeline/run.js');const session=await loadGenerationSession();await runAvatarPipeline({imageFile:session.imageFile,providerConfig:{provider:'volcengine',apiKey:'offline-fixture',model:'doubao-seedance-2-0-fast-260128'},automatic:true,session,checkpoint:saveGenerationSession});});
 assert.deepEqual(counts,before);
 rejectReference=true;
 const failure=await page.evaluate(async png=>{const {runAvatarPipeline}=await import('/src/lib/avatar-pipeline/run.js');try{await runAvatarPipeline({imageFile:new File([Uint8Array.from(atob(png),c=>c.charCodeAt(0))],'fixture.png',{type:'image/png'}),providerConfig:{provider:'volcengine',apiKey:'offline-fixture',model:'doubao-seedance-2-0-fast-260128'},automatic:true,session:{actions:{}},checkpoint:async()=>{}});return '';}catch(e){return e.message;}},png);
 assert.match(failure,/参考图自动检查未通过/);assert.equal(counts.videos,11);assert.equal(errors.length,0,errors.join('\n'));
 fs.writeFileSync(dir+'/result.json',JSON.stringify({automatic11:first,reloadNoPaidReplay:true,referenceRejectedBeforeVideos:true,externalCallsMocked:true,errors},null,2));
 console.log('PASS: automatic 11 clips, real local decoding, durable restart, no paid replay, rejected-reference stop.');
}finally{await browser.close();}
