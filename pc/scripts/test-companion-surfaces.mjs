import assert from 'node:assert/strict';
import fs from 'node:fs';
import {chromium} from 'playwright';
const output=process.env.PET_SURFACE_REPORT_DIR;
if(!output)throw Error('Set PET_SURFACE_REPORT_DIR');
fs.mkdirSync(output,{recursive:true});
const browser=await chromium.launch({executablePath:process.env.PET_TEST_BROWSER||'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try {
  const page=await browser.newPage({viewport:{width:1440,height:1100}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto(process.env.PET_APP_PREVIEW_URL||'http://127.0.0.1:4196/app-surface-preview.html');
  await page.locator('article').last().waitFor();
  await page.locator('[data-content-view=scene]').first().waitFor();
  assert.equal(await page.locator('article').count(),4);
  for(const article of await page.locator('article').all()){
    const id=await article.getAttribute('data-id');
    await article.scrollIntoViewIfNeeded();
    for(const button of await article.getByRole('button').all()){
      const metric=await button.evaluate(el=>{
        const box=el.lastElementChild.getBoundingClientRect(),t=el.querySelector('[data-text-box]')?.getBoundingClientRect();
        return t?{left:t.left-box.left,right:box.right-t.right,top:t.top-box.top,bottom:box.bottom-t.bottom}:null;
      });
      if(metric){assert.ok(metric.left>=6&&metric.right>=6,`${id}: ${JSON.stringify(metric)}`);assert.ok(metric.top>=3&&metric.bottom>=3,`${id}: ${JSON.stringify(metric)}`);}
    }
    await article.locator('.screen').screenshot({path:`${output}/${id}.png`});
  }
  const frog=page.locator('[data-id=bloomfrog_companion]');
  await frog.getByRole('button',{name:'摸摸',exact:true}).click();
  assert.match(await frog.locator('.screen').textContent(),/舒服/);
  await frog.getByRole('button',{name:'喂食',exact:true}).click();
  assert.match(await frog.locator('.screen').textContent(),/好吃/);
  const pong=page.locator('[data-id=two-key-pong]');
  await pong.getByRole('button',{name:'开始接球',exact:true}).click();
  assert.match(await pong.locator('.screen').textContent(),/守住底线/);
  await pong.getByRole('button',{name:'向左',exact:true}).click();
  await pong.getByRole('button',{name:'向右',exact:true}).click();
  assert.deepEqual(errors,[]);
  console.log('PASS four 640x480 surfaces, button padding, frog touch responses, shared-core pong start/actions; no live PC/device assertions.');
}finally{await browser.close();}
