// The Rust service exports synthetic records from its real isolated atomic store.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {chromium} from 'playwright';
const fixtures=JSON.parse(fs.readFileSync(process.env.PET_TODO_UI_FIXTURES,'utf8'));
const output=process.env.PET_UI_REPORT_DIR;
assert(output);fs.mkdirSync(output,{recursive:true});
const base=process.env.PET_APP_PREVIEW_URL||'http://127.0.0.1:4202';
const browser=await chromium.launch({executablePath:process.env.PET_TEST_BROWSER||'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try{
  const page=await browser.newPage({viewport:{width:800,height:600}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto(`${base}/tests/manual/host-surface.html`);
  await page.waitForFunction(()=>Boolean(window.__hostSurface));
  const item=await page.evaluate(async()=> (await (await fetch('/__app_fixtures')).json()).find(p=>p.manifest.id==='upcoming-todos'));
  assert(item);
  const screen=page.locator('.screen');
  for(const fixture of fixtures){
    await page.evaluate(({item,fixture})=>window.__hostSurface.show(item,fixture),{item,fixture});
    await screen.waitFor();await page.waitForTimeout(80);
    for(const button of await screen.locator('g[role=button]').all()){
      const b=await button.boundingBox();assert(b.width>=44&&b.height>=44,`${fixture.name}: touch target`);
    }
    await screen.screenshot({path:path.join(output,`${fixture.name}.png`)});
    if(fixture.name==='list'){
      // Text is drawn as glyph images, so use the exact accessible label.
      const first=screen.getByRole('button',{name:fixture.values.eyebrow,exact:true});
      await first.click();
      assert.equal((await page.evaluate(()=>window.__hostSurface.calls)).at(-1).action,'host.row_a');
    }
    if(fixture.name==='detail'||fixture.name==='completed'){
      const button=screen.getByRole('button',{name:fixture.values.metricUnit,exact:true});
      await button.click();assert.equal(await screen.getByRole('dialog').count(),1);
      assert.equal((await page.evaluate(()=>window.__hostSurface.calls)).length,0);
      await screen.getByRole('button',{name:'取消',exact:true}).click();
      assert.equal((await page.evaluate(()=>window.__hostSurface.calls)).length,0);
      await button.click();await screen.getByRole('button',{name:'确认',exact:true}).click();
      assert.deepEqual(await page.evaluate(()=>window.__hostSurface.calls),[{action:'host.toggle',confirm:true,context:fixture.ui.context}]);
    }
    if(fixture.name==='empty'){
      assert.equal(await screen.getByRole('button',{name:'查看全文',exact:true}).getAttribute('aria-disabled'),'true');
    }
  }
  const detail=fixtures.find(f=>f.name==='detail');
  await page.evaluate(({item,fixture})=>window.__hostSurface.show(item,fixture),{item,fixture:detail});
  await page.waitForTimeout(60);
  await screen.getByRole('button',{name:detail.values.metricUnit,exact:true}).click();
  assert.equal(await screen.getByRole('dialog').count(),1);
  await page.evaluate(({item,fixture})=>window.__hostSurface.show(item,fixture),{item,fixture:detail});
  await page.waitForTimeout(60);
  assert.equal(await screen.getByRole('dialog').count(),1,'unchanged host heartbeat preserves pending confirmation');
  const changed={...detail,ui:{...detail.ui,context:'b'.repeat(64)}};
  await page.evaluate(({item,fixture})=>window.__hostSurface.show(item,fixture),{item,fixture:changed});
  await page.waitForTimeout(60);
  assert.equal(await screen.getByRole('dialog').count(),0,'changing displayed record cancels its old confirmation');
  assert.deepEqual(await page.evaluate(()=>window.__hostSurface.calls),[]);
  for(const mode of [6,7]){
    await page.evaluate(({item,mode})=>window.__hostSurface.show(item,{values:{headline:'电脑暂未连接',note:'请保持 Pet Manager 运行'},ui:{mode,actions:[]}}),{item,mode});
    await page.waitForTimeout(60);
    for(const b of await screen.getByRole('button').all())assert.equal(await b.getAttribute('aria-disabled'),'true');
    await screen.screenshot({path:path.join(output,`mode-${mode}.png`)});
  }
  assert.deepEqual(errors,[]);
  fs.writeFileSync(path.join(output,'report.json'),JSON.stringify({snapshots:fixtures.length,errors,confirmation:'cancel/no mutation; confirm/exact token',touchTargets:'>=44px',hardware:'not run'},null,2));
  console.log(`PASS ${fixtures.length} real-service fixture states, 44px touch targets, confirmation/cancel, busy/offline gating. Device acceptance pending.`);
}finally{await browser.close();}
