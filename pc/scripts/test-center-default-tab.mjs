// Isolated fixture only: mock IPC, no device/community or user profile access.
import assert from 'node:assert/strict';
import {chromium} from 'playwright';
const browser=await chromium.launch({executablePath:process.env.PET_TEST_BROWSER||'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try {
  const page=await browser.newPage({viewport:{width:1440,height:1000}});
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('http://127.0.0.1:4206/tests/component-center-review.html');
  const tabs=page.getByRole('tab');
  await page.getByRole('tab',{name:'我的组件',exact:true}).waitFor();
  assert.deepEqual((await tabs.allTextContents()).slice(0,2),['我的组件','社区广场']);
  assert.equal(await tabs.first().getAttribute('aria-selected'),'true');
  assert.equal(await page.locator('body').innerText().then(s=>s.includes('官方')),false);
  for(const name of ['云端跳一跳','每日黄历','每天一本书'])
    assert.equal(await page.locator('.candidate-card__name').filter({hasText:name}).count(),1);
  await page.getByRole('tab',{name:'社区广场',exact:true}).click();
  assert.equal(await tabs.nth(1).getAttribute('aria-selected'),'true');
  await tabs.first().click();
  await page.reload();
  await page.getByRole('tab',{name:'我的组件',exact:true}).waitFor();
  assert.equal(await page.getByRole('tab',{name:'我的组件',exact:true}).getAttribute('aria-selected'),'true');
  assert.deepEqual(errors,[]);
  if(process.env.PET_CENTER_SCREENSHOT)await page.screenshot({path:process.env.PET_CENTER_SCREENSHOT,fullPage:false});
  console.log('PASS: local default, community second, no maintainer badge, three new packages, reload, no render errors');
} finally {await browser.close();}
