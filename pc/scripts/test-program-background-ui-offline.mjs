// Real React controls with an injected typed-response fixture. No device/IPC.
import {mkdirSync,writeFileSync} from 'node:fs';
import {resolve,join} from 'node:path';
import assert from 'node:assert/strict';
import {chromium} from 'playwright';
const [directory,label='run']=process.argv.slice(2);
if(!directory||!/^[\w-]+$/.test(label))throw Error('explicit_output_required');
const out=resolve(directory);mkdirSync(out,{recursive:true});
const url=new URL(process.env.PET_BACKGROUND_UI_URL||'http://127.0.0.1:4198/tests/manual/program-background.html');
if(url.protocol!=='http:'||url.hostname!=='127.0.0.1')throw Error('loopback_only');
const browser=await chromium.launch({headless:true,...(process.env.PET_TEST_BROWSER?{executablePath:process.env.PET_TEST_BROWSER}:{})});
const page=await browser.newPage({viewport:{width:1100,height:850},serviceWorkers:'block'}),results=[],errors=[],blocked=[];
await page.route('**/*',route=>{const u=new URL(route.request().url());if(u.origin===url.origin||u.protocol==='data:')return route.continue();blocked.push(u.origin);return route.abort('blockedbyclient');});
await page.routeWebSocket('**/*',route=>{const u=new URL(route.url());if(u.hostname!=='127.0.0.1')blocked.push(u.origin);route.close();});
page.on('pageerror',error=>errors.push(error.message));
const run=async(name,fn)=>{await fn();results.push({name,passed:true});console.log('PASS '+name);};
const count=name=>page.evaluate(name=>window.__backgroundUi.calls.filter(c=>c.name===name).length,name);
const start=()=>page.getByRole('button',{name:'在设备后台运行',exact:true});
const stop=()=>page.getByRole('button',{name:'停止',exact:true});
const refresh=()=>page.getByRole('button',{name:'刷新状态',exact:true});
try{
  assert.equal((await page.goto(url.href))?.status(),200);
  await page.waitForFunction(()=>window.__backgroundUi?.ready);await start().waitFor();await page.waitForFunction(()=>[...document.querySelectorAll('button')].some(b=>b.textContent==='在设备后台运行'&&!b.disabled));
  assert.equal(await page.evaluate(()=>!!window.__TAURI_INTERNALS__),false);
  await run('initial mount only observes and one explicit start controls the native version',async()=>{
    assert.equal(await count('usb_start_background_program'),0);await start().click();
    await page.getByRole('button',{name:'已在后台运行',exact:true}).waitFor();assert.equal(await count('usb_start_background_program'),1);
    assert.equal(await page.getByRole('button',{name:'已在后台运行',exact:true}).isDisabled(),true);
  });
  await run('stopping retains exact u64 owner and never resumes or polls stopped state',async()=>{
    await stop().click();await start().waitFor();const before=await count('usb_get_background_programs');await page.waitForTimeout(2200);
    assert.equal(await count('usb_get_background_programs'),before);assert.equal(await count('usb_start_background_program'),1);
    assert.equal(await page.evaluate(()=>window.__backgroundUi.calls.find(c=>c.name==='usb_stop_background_program').args.registration),'18446744073709551615');
  });
  await run('unknown start is not replayed and only an explicit refresh reconciles it',async()=>{
    await page.evaluate(()=>{window.__backgroundUi.nextUnknown=true;});await start().click();
    await page.getByText('上次结果未确认，未自动重发。请先刷新设备状态。',{exact:true}).first().waitFor();
    const before=await count('usb_get_background_programs');await page.waitForTimeout(2200);assert.equal(await count('usb_get_background_programs'),before);
    assert.equal(await count('usb_start_background_program'),2);assert.equal(await start().isDisabled(),true);
    await page.screenshot({path:join(out,`${label}-unknown.png`)});await refresh().click();await page.getByRole('button',{name:'已在后台运行',exact:true}).waitFor();
    assert.equal(await count('usb_start_background_program'),2);await stop().click();await start().waitFor();
  });
  await run('hiding cancels observation but not the intentionally running program',async()=>{
    await start().click();await page.getByRole('button',{name:'已在后台运行',exact:true}).waitFor();await page.evaluate(()=>window.__backgroundUi.setActive(false));
    await page.getByRole('region',{name:'设备后台程序'}).waitFor({state:'hidden'});const before=await count('usb_get_background_programs');await page.waitForTimeout(2200);
    assert.equal(await count('usb_get_background_programs'),before);assert.equal(await page.evaluate(()=>window.__backgroundUi.live),true);
    await page.evaluate(()=>window.__backgroundUi.setActive(true));await page.getByRole('button',{name:'已在后台运行',exact:true}).waitFor();assert.equal(await count('usb_start_background_program'),3);
    await stop().click();await start().waitFor();
  });
  await run('legacy capabilities create neither panels nor IPC',async()=>{
    await page.evaluate(()=>window.__backgroundUi.setCompatible(false));await page.getByRole('region',{name:'设备后台程序'}).waitFor({state:'hidden'});
    const before=await page.evaluate(()=>window.__backgroundUi.calls.length);await page.waitForTimeout(200);assert.equal(await page.evaluate(()=>window.__backgroundUi.calls.length),before);
    await page.evaluate(()=>window.__backgroundUi.setCompatible(true));await start().waitFor();
  });
  await run('late old-boot query cannot overwrite the new owner',async()=>{
    await page.waitForFunction(()=>[...document.querySelectorAll('button')].some(b=>b.textContent==='刷新状态'&&!b.disabled));
    await page.evaluate(()=>{window.__backgroundUi.delayNextQuery=250;});await refresh().click();await page.evaluate(()=>window.__backgroundUi.bumpBoot());
    await page.getByText(/修订 99/).waitFor();await page.waitForTimeout(300);assert((await page.getByRole('region',{name:'设备后台程序'}).textContent()).includes('修订 99'));
    await page.screenshot({path:join(out,`${label}-stopped.png`)});
  });
  assert.deepEqual(errors,[]);assert.deepEqual(blocked,[]);
  writeFileSync(join(out,`${label}.json`),JSON.stringify({passed:results.length,results,pageErrors:errors,blockedExternalRequests:blocked,fixture:url.href,tauri:false,hardware:false,audioUsed:false,userData:false},null,2));
}catch(error){
  await page.screenshot({path:join(out,`${label}-failure.png`)}).catch(()=>{});
  writeFileSync(join(out,`${label}.json`),JSON.stringify({passed:results.length,results,error:String(error.stack),pageErrors:errors,blockedExternalRequests:blocked},null,2));throw error;
}finally{await browser.close();}
