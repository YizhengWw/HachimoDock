import {chromium} from 'playwright';
import assert from 'node:assert/strict';
const browser=await chromium.launch({executablePath:'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try{
  const page=await browser.newPage({viewport:{width:960,height:880}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto('http://127.0.0.1:4203/new-components-preview.html?component=hop');
  const screen=page.locator('.screen');await screen.locator('[data-entity="puppy"]').waitFor();
  await screen.screenshot({path:'new-components/qa/track-ready.png'});
  const button=screen.getByRole('button',{name:'按住开始',exact:true});
  const box=await button.boundingBox();await page.mouse.move(box.x+box.width/2,box.y+box.height/2);
  await page.mouse.down();await page.waitForTimeout(680);await page.mouse.up();await page.waitForTimeout(1300);
  let x=Number(await screen.locator('[data-entity="puppy"]').getAttribute('data-x'));
  assert(x>3.9&&x<4.1,`camera follow line, not reset to x=2: ${x}`);
  await screen.screenshot({path:'new-components/qa/track-first-landing.png'});
  // Four subsequent jumps selected from the visible platform geometry.
  for(let i=0;i<4;i++){
    const es=await screen.locator('[data-entity]').evaluateAll(es=>es.map(e=>({id:e.dataset.entity,x:+e.dataset.x,w:+e.dataset.width})));
    const b=es.find(e=>e.id==='puppy'),ps=es.filter(e=>e.id!=='puppy').sort((a,b)=>a.x-b.x);
    const current=ps.find(p=>p.x<=b.x+1&&b.x+1<p.x+p.w),next=ps.find(p=>p.x>=current.x+current.w);
    const distance=next.x+next.w/2-b.x-1;
    const ms=Math.max(0,Math.min(1200,(distance/.76-2)/13*1200));
    const btn=await screen.getByRole('button',{name:'按住蓄力',exact:true}).boundingBox();
    await page.mouse.move(btn.x+btn.width/2,btn.y+btn.height/2);await page.mouse.down();await page.waitForTimeout(ms);await page.mouse.up();await page.waitForTimeout(1300);
    assert.equal(Number(await screen.locator('[data-entity="puppy"]').getAttribute('data-y')),4,'landed, not a fake screen transition');
  }
  await screen.screenshot({path:'new-components/qa/track-five-landings.png'});
  await screen.getByRole('button',{name:'暂停',exact:true}).click();
  const held=await screen.locator('[data-entity]').evaluateAll(es=>es.map(e=>e.outerHTML));await page.waitForTimeout(450);
  assert.deepEqual(await screen.locator('[data-entity]').evaluateAll(es=>es.map(e=>e.outerHTML)),held);
  await screen.getByRole('button',{name:'继续',exact:true}).click();
  assert.deepEqual(errors,[]);console.log('PASS browser pointer hold/release, 5 consecutive landings, camera following without reset, pause/resume, zero page errors.');
}finally{await browser.close();}
