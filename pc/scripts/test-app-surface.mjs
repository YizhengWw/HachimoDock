// Real package UI regression; this never enables services or touches user saves.
import assert from 'node:assert/strict';
import {chromium} from 'playwright';
const browser=await chromium.launch({executablePath:process.env.PET_TEST_BROWSER||'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try{
  const page=await browser.newPage({viewport:{width:1440,height:1100}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto(process.env.PET_APP_PREVIEW_URL||'http://127.0.0.1:4192/app-surface-preview.html');
  await page.locator('article').last().waitFor();assert.equal(await page.locator('article').count(),8);
  let checked=0;
  for(const article of await page.locator('article').all()){
    assert.ok(await article.locator('svg > g').count()>0);
    for(const button of await article.locator('svg > g[role=button]').all()){
      // Furnishings are direct-touch images, not text/icon buttons.
      if(await button.locator('image').count()&&!await button.locator('[data-text-box]').count())continue;
      const m=await button.evaluate(el=>{
        const hit=el.lastElementChild.getBoundingClientRect(),text=el.querySelector('[data-text-box]')||el.querySelector('text'),t=text.getBoundingClientRect();
        const poly=el.querySelector('polyline'),i=poly?.parentElement.getBoundingClientRect();
        return {label:el.querySelector('[data-label]')?.getAttribute('data-label')||text.textContent,left:t.left-hit.left,right:hit.right-t.right,top:t.top-hit.top,bottom:hit.bottom-t.bottom,icon:Boolean(poly),gap:i?t.left-i.right:null,cx:((i?i.left:t.left)+t.right)/2-(hit.left+hit.right)/2};
      });
      if(!['上一站','下一站'].includes(m.label))assert.ok(m.icon,`missing button icon: ${m.label}`);
      assert.ok(m.left>=10&&m.right>=10,`horizontal padding: ${JSON.stringify(m)}`);
      assert.ok(m.top>=5&&m.bottom>=5,`vertical centering: ${JSON.stringify(m)}`);
      if(m.icon)assert.ok(m.gap>=5&&m.gap<=16,`icon gap: ${JSON.stringify(m)}`);
      assert.ok(Math.abs(m.cx)<=3,`group centering: ${JSON.stringify(m)}`);
      assert.ok(!m.label.includes('...'),`default button clipped: ${m.label}`);checked++;
    }
    assert.ok((await article.locator('svg').textContent()).includes('SW1'));
    assert.ok((await article.locator('svg').textContent()).includes('SW2'));
  }
  const home=page.locator('[data-id=hachimi-home]');
  const pat=home.getByRole('button',{name:'摸摸头',exact:true});
  await pat.scrollIntoViewIfNeeded();const hit=await pat.boundingBox();
  await page.mouse.move(hit.x+hit.width/2,hit.y+hit.height/2);await page.mouse.down();
  await page.waitForTimeout(760);await page.mouse.up();
  assert.doesNotMatch(await home.locator('svg').textContent(),/摸摸收到啦/,'long press must not click');
  await page.mouse.down();await page.mouse.move(hit.x+hit.width/2+50,hit.y+hit.height/2);await page.mouse.up();
  assert.doesNotMatch(await home.locator('svg').textContent(),/摸摸收到啦/,'drag must not click');
  await home.getByRole('button',{name:'摸摸头',exact:true}).click();assert.match(await home.locator('svg').textContent(),/摸摸收到啦/);
  await home.getByRole('button',{name:'布置小屋',exact:true}).click();assert.match(await home.locator('svg').textContent(),/选一株/);
  const plant=home.getByRole('button',{name:'previous',exact:true});
  const before=await plant.locator('image').getAttribute('x');
  await home.getByRole('button',{name:'换一株',exact:true}).click();
  assert.notEqual(await plant.locator('image').getAttribute('x'),before);
  await home.getByRole('button',{name:'选摆件',exact:true}).click();
  await home.getByRole('button',{name:'换一件',exact:true}).click();
  await home.getByRole('button',{name:'调位置',exact:true}).click();
  await home.getByRole('button',{name:'换个位置',exact:true}).click();
  await home.getByRole('button',{name:'完成布置',exact:true}).click();
  assert.match(await home.locator('svg').textContent(),/今天，也一起/);
  const garden=page.locator('[data-id=hachimi-garden]');
  await garden.getByRole('button',{name:'开始专注',exact:true}).click();await garden.getByRole('button',{name:'暂停',exact:true}).click();
  assert.match(await garden.locator('svg').textContent(),/休息一下/);assert.deepEqual(errors,[]);
  console.log(`PASS: 8 retained package layouts, ${checked} buttons, padding/centering/keycaps, live actions, no browser errors.`);
}finally{await browser.close();}
