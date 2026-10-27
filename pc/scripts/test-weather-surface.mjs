// Real package geometry. Host state transitions are covered separately in Rust/HIL.
import assert from 'node:assert/strict';
import {chromium} from 'playwright';
const browser=await chromium.launch({executablePath:'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try{
  const page=await browser.newPage({viewport:{width:1000,height:1000}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto(process.env.PET_APP_PREVIEW_URL||'http://127.0.0.1:4195/app-surface-preview.html');
  const surface=page.locator('[data-id=hachimi-weather] .screen');
  await surface.getByRole('button',{name:'选择城市',exact:true}).waitFor();
  for(const label of ['选择城市','更新预报','近期待办','上一城','下一城']){
    const button=surface.getByRole('button',{name:label,exact:true});
    const box=await button.boundingBox();assert(box.width>=44&&box.height>=44,label);
    const m=await button.evaluate(el=>{
      const hit=el.lastElementChild.getBoundingClientRect();
      const t=el.querySelector('[data-text-box]').getBoundingClientRect();
      return {left:t.left-hit.left,right:hit.right-t.right,top:t.top-hit.top,bottom:hit.bottom-t.bottom};
    });
    assert(m.left>=10&&m.right>=10&&m.top>=5&&m.bottom>=5,`${label}: ${JSON.stringify(m)}`);
    await button.click(); // package hit area must be enabled and not obscured
  }
  assert.equal(await surface.locator('[data-label="左右选城"]').count(),1);
  assert.deepEqual(errors,[]);
  if(process.env.PET_WEATHER_SCREENSHOT)await surface.screenshot({path:process.env.PET_WEATHER_SCREENSHOT});
  console.log('PASS weather location entry, five 44px+ touch targets, label padding, footer, no browser errors. Host/physical touch tested separately.');
}finally{await browser.close();}
