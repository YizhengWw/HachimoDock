import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {chromium} from 'playwright';
const output=process.env.PET_NEW_COMPONENT_QA||'new-components/qa';fs.mkdirSync(output,{recursive:true});
const browser=await chromium.launch({executablePath:process.env.PET_TEST_BROWSER||'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try{
  const page=await browser.newPage({viewport:{width:960,height:880}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  for(const [component,views] of [
    ['calendar',[['today','春节当天'],['details','宜忌详情'],['detail-page-2','详情第二页'],['leap-month','闰月'],['solar-term','清明节气']]],
    ['books',[['reading','阅读首页'],['second','阅读第二页'],['last','阅读末页'],['category','分类选择']]],
  ]){
  await page.goto(`http://127.0.0.1:4203/new-components-preview.html?component=${component}`);
  for(const [key,label] of views){
    await page.locator('nav').getByRole('button',{name:label,exact:true}).click();
    const screen=page.locator('.screen');await screen.locator('svg').waitFor();
    await page.evaluate(async()=>{await document.fonts.ready;await Promise.all([...document.images].map(i=>i.decode().catch(()=>{})));});
    const labels=await screen.locator('[data-label]').evaluateAll(es=>es.map(e=>e.getAttribute('data-label')));
    assert(!labels.some(s=>s.includes('...')),`${key}: clipped text: ${labels.join('|')}`);
    assert(labels.join('').includes('全局键退出'));
    for(const button of await screen.getByRole('button').all()){
      const b=await button.boundingBox();assert(b.width>=44&&b.height>=44,'touch target too small');
      const padding=await button.evaluate(el=>{const a=el.getBoundingClientRect(),t=el.querySelector('[data-text-box]').getBoundingClientRect();return [t.left-a.left,a.right-t.right,t.top-a.top,a.bottom-t.bottom];});
      assert(padding.every(p=>p>=5),`${key}: button text padding ${padding}`);
    }
    await screen.screenshot({path:path.join(output,`${component}-${key}.png`)});
  }
  }
  assert.deepEqual(errors,[]);console.log('PASS nine production calendar/book snapshots; native package text, no ellipses, >=44px touch targets, >=5px label padding, no JS errors.');
}finally{await browser.close();}
