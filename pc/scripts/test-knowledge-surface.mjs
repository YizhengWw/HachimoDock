// Real package + production Rust-generated snapshots; no user state or audio.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {chromium} from 'playwright';
const views=JSON.parse(fs.readFileSync(process.env.PET_KNOWLEDGE_VIEWS,'utf8'));
const browser=await chromium.launch({executablePath:process.env.PET_TEST_BROWSER||'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try{
  const page=await browser.newPage({viewport:{width:900,height:1000}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  let selected='reading';
  await page.route('**/__app_fixtures',async route=>{
    const response=await route.fetch(),items=await response.json();
    const item=items.find(x=>x.manifest.id==='hachimi-learn');assert(item);
    item.runtime.dashboard={...item.runtime.dashboard,...views[selected]};
    await route.fulfill({response,json:[item]});
  });
  for(const state of ['reading','second','category','changed']){
    selected=state;
    await page.goto(process.env.PET_APP_PREVIEW_URL||'http://127.0.0.1:4196/app-surface-preview.html');
    const surface=page.locator('[data-id=hachimi-learn] .screen');
    await surface.getByRole('button',{name:views[state].eyebrow,exact:true}).waitFor();
    const labels=await surface.locator('[data-label]').evaluateAll(es=>es.map(e=>e.getAttribute('data-label')));
    assert(!labels.some(s=>s.includes('...')),'text clipped in '+state);
    const text=labels.join('');
    for(const field of ['headline','note','footer','badge','metricLabel'])assert(text.includes(views[state][field]),`missing ${field} in ${state}`);
    assert.equal(await surface.getByRole('button').count(),2);
    assert(!labels.some(s=>/收藏|读本页|上一项|下一项/.test(s)));
    assert(text.includes('左右翻页')&&text.includes('全局键退出'));
    for(const button of await surface.getByRole('button').all()){
      const b=await button.boundingBox();assert(b.width>=44&&b.height>=44);
      const m=await button.evaluate(el=>{
        const hit=el.lastElementChild.getBoundingClientRect(),t=el.querySelector('[data-text-box]').getBoundingClientRect();
        return {left:t.left-hit.left,right:hit.right-t.right,top:t.top-hit.top,bottom:hit.bottom-t.bottom};
      });
      assert(m.left>=10&&m.right>=10&&m.top>=5&&m.bottom>=5,`${state}: ${JSON.stringify(m)}`);
      await button.click(); // verifies target enabled/visible, not the host state machine
    }
    await page.evaluate(async()=>Promise.all([...document.images].map(i=>i.decode().catch(()=>{}))));
    if(process.env.PET_KNOWLEDGE_SCREENSHOTS)await surface.screenshot({path:`${process.env.PET_KNOWLEDGE_SCREENSHOTS}/${state}.png`});
  }
  assert.deepEqual(errors,[]);
  console.log('PASS: production reading/category/page snapshots, two touch targets, no removed controls, complete text without ellipses, label padding, footer, no browser errors. Physical touch remains separate.');
}finally{await browser.close();}
