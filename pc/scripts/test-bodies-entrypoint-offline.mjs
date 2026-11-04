import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createServer} from 'vite';
import react from '@vitejs/plugin-react';
import {chromium} from 'playwright';

test('actual AppSurface bodies entrypoint pauses, resumes and disposes its real WASM lifecycle',{timeout:60000},async()=>{
  const temporary=await mkdtemp(join(tmpdir(),'bodies-entrypoint-'));
  let server,browser;
  try{
    server=await createServer({configFile:false,root:fileURLToPath(new URL('..',import.meta.url)),cacheDir:join(temporary,'cache'),plugins:[react()],server:{host:'127.0.0.1',port:0},logLevel:'error'});
    await server.listen();const address=server.httpServer.address(),origin=`http://127.0.0.1:${address.port}`;
    browser=await chromium.launch({headless:true,...(process.env.PET_TEST_BROWSER?{executablePath:process.env.PET_TEST_BROWSER}:{})});
    const page=await browser.newPage({viewport:{width:900,height:720}}),errors=[];
    page.on('pageerror',e=>errors.push(e.message));
    await page.route('**/*',route=>{const u=new URL(route.request().url());return u.origin===origin||u.protocol==='data:'?route.continue():route.abort();});
    await page.addInitScript(()=>{
      const intervals=new Map();let id=0,instantiations=0;
      const realFetch=window.fetch.bind(window),instantiate=WebAssembly.instantiate.bind(WebAssembly);
      window.setInterval=(callback,ms)=>{intervals.set(++id,{callback,ms});return id;};window.clearInterval=id=>intervals.delete(id);
      WebAssembly.instantiate=async(...args)=>{instantiations++;return instantiate(...args);};
      window.fetch=async(...args)=>{const response=await realFetch(...args);if(window.__delayBodyFetch&&String(args[0]).includes('classic-core.wasm'))await new Promise(resolve=>{window.__releaseBodyFetch=resolve;});return response;};
      window.__bodyClock={count:()=>[...intervals.values()].filter(t=>t.ms===50).length,instances:()=>instantiations,advance:async(n)=>{for(let i=0;i<n;i++){for(const {callback,ms}of [...intervals.values()])if(ms===50)callback();await new Promise(resolve=>setTimeout(resolve,0));}}};
    });
    await page.goto(`${origin}/tests/manual/bodies-entrypoint.html`);
    await page.waitForFunction(()=>window.__bodiesEntry?.snapshot().length===2);
    const advance=n=>page.evaluate(n=>window.__bodyClock.advance(n),n),snapshot=()=>page.evaluate(()=>window.__bodiesEntry.snapshot());
    await advance(3);const moving=await snapshot();assert.ok(moving[0].x>2);
    await page.getByRole('button',{name:'Reset',exact:true}).click();await page.getByRole('dialog').waitFor();
    const confirmedPause=await snapshot();await advance(100);assert.deepEqual(await snapshot(),confirmedPause,'confirmation must hold bodies through arbitrarily many UI ticks');
    await page.getByRole('button',{name:'取消',exact:true}).click();await advance(1);
    const resumed=await snapshot();assert.notDeepEqual(resumed,confirmedPause);assert.ok(Math.abs(resumed[0].x-confirmedPause[0].x)<=0.3,'resume must not replay the paused interval');
    await page.getByRole('button',{name:'Pause',exact:true}).click();const statePaused=await snapshot();await advance(100);assert.deepEqual(await snapshot(),statePaused);
    await page.getByRole('button',{name:'Pause',exact:true}).click();await advance(1);assert.notDeepEqual(await snapshot(),statePaused);
    await page.evaluate(()=>window.__bodiesEntry.active(false));assert.equal(await page.evaluate(()=>window.__bodyClock.count()),0);
    const inactive=await snapshot();await advance(100);assert.deepEqual(await snapshot(),inactive);
    await page.evaluate(()=>window.__bodiesEntry.active(true));await advance(1);assert.notDeepEqual(await snapshot(),inactive);
    await page.getByRole('button',{name:'Reset',exact:true}).click();await page.getByRole('button',{name:'确认',exact:true}).click();assert.equal((await snapshot())[0].x,2);
    await page.evaluate(()=>window.__bodiesEntry.unmount());assert.equal(await page.evaluate(()=>window.__bodyClock.count()),0);await advance(100);assert.deepEqual(await snapshot(),[]);
    await page.evaluate(()=>{window.__delayBodyFetch=true;window.__bodiesEntry.mount();});await page.waitForFunction(()=>!!window.__releaseBodyFetch);
    const beforeLate=await page.evaluate(()=>window.__bodyClock.instances());await page.evaluate(()=>window.__bodiesEntry.unmount());
    await page.evaluate(()=>{window.__delayBodyFetch=false;window.__releaseBodyFetch();});await page.waitForTimeout(40);
    assert.equal(await page.evaluate(()=>window.__bodyClock.instances()),beforeLate,'disposed pending loads must not instantiate WASM');assert.equal(await page.evaluate(()=>window.__bodyClock.count()),0);assert.deepEqual(await snapshot(),[]);
    await page.evaluate(()=>window.__bodiesEntry.mount());await page.waitForFunction(()=>window.__bodiesEntry.snapshot().length===2);assert.equal((await snapshot())[0].x,2);
    await advance(3);await page.evaluate(()=>window.__bodiesEntry.replace());await page.waitForFunction(()=>window.__bodiesEntry.snapshot()[0]?.x===2);assert.equal(await page.evaluate(()=>window.__bodyClock.count()),1);
    assert.deepEqual(errors,[]);
  }finally{await browser?.close();await server?.close();await rm(temporary,{recursive:true,force:true});}
});
