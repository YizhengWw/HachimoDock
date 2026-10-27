// Isolated browser regression: no real community writes or user profile access.
import assert from 'node:assert/strict';
import {mkdirSync,writeFileSync} from 'node:fs';
import {join} from 'node:path';
import {chromium} from 'playwright';
const output=process.env.PET_UI_REPORT_DIR;
assert(output,'PET_UI_REPORT_DIR required');mkdirSync(output,{recursive:true});
const browser=await chromium.launch({executablePath:process.env.PET_TEST_BROWSER||'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try{
  const page=await browser.newPage({viewport:{width:1180,height:980}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));
  await page.goto(process.env.PET_COMMENT_TEST_URL||'http://127.0.0.1:4214/tests/component-center-review.html');
  async function detail(){await page.getByRole('tab',{name:'社区广场',exact:true}).click();await page.getByRole('button',{name:/社区回归样例/}).first().click();await page.getByRole('heading',{name:'评论',exact:true}).waitFor();}
  await detail();
  const dialog=page.getByRole('dialog',{name:'玩法详情'}),form=dialog.locator('.community-comment-form');
  const code=await form.getByLabel('评论身份').innerText();assert.match(code,/^用户-[a-f0-9]{12}$/);
  assert.equal(await dialog.getByText('公开显示名',{exact:true}).count(),0);
  assert.equal(await dialog.getByText('还没有评论，说说你的体验吧。',{exact:true}).count(),0);
  assert.equal(await form.getByRole('checkbox').count(),0);
  await form.screenshot({path:join(output,'comment-anonymous.png')});
  const calls=()=>page.evaluate(()=>window.__reviewCalls.filter(x=>x.command==='community_request'&&x.args.input.action==='comment').map(x=>x.args.input));
  await form.getByRole('button',{name:'编辑昵称'}).click();await form.getByLabel('昵称',{exact:true}).fill('取消昵称');
  await form.getByLabel('昵称',{exact:true}).press('Escape');assert.equal(await dialog.count(),1);assert.equal(await form.getByLabel('评论身份').innerText(),code);
  await form.getByRole('button',{name:'编辑昵称'}).click();await form.getByLabel('昵称',{exact:true}).fill('   ');await form.getByRole('button',{name:'保存',exact:true}).click();
  await form.getByRole('alert').filter({hasText:'请输入昵称'}).waitFor();
  await form.getByLabel('昵称',{exact:true}).fill('  橘子汽水  ');await form.getByLabel('昵称',{exact:true}).press('Enter');
  assert.equal((await calls()).length,0,'Enter in nickname must not post');assert.equal(await form.getByLabel('评论身份').innerText(),'橘子汽水');
  await form.getByLabel('评论内容').fill('  很好玩，切换也很方便。  ');await form.getByRole('button',{name:'发表评论'}).click();
  await dialog.locator('.community-comments article').first().waitFor();
  assert.equal((await calls())[0].name,'橘子汽水');assert.equal((await calls())[0].text,'很好玩，切换也很方便。');
  assert.equal(await form.getByLabel('评论内容').inputValue(),'');
  await page.evaluate(()=>{window.__failCommentOnce=true;});await form.getByLabel('评论内容').fill('模拟回执中断重试');await form.getByRole('button',{name:'发表评论'}).click();
  await dialog.getByRole('alert').filter({hasText:'模拟回执中断'}).waitFor();assert.equal(await form.getByLabel('评论内容').inputValue(),'模拟回执中断重试');
  await form.getByRole('button',{name:'发表评论'}).click();await dialog.locator('.community-comments article').nth(1).waitFor();
  const sent=await calls();assert.equal(sent.at(-1).requestId,sent.at(-2).requestId);assert.equal(await dialog.locator('.community-comments article').count(),2);
  await form.screenshot({path:join(output,'comment-nickname.png')});
  await page.reload();await detail();assert.equal(await page.getByLabel('评论身份').innerText(),'橘子汽水');
  await page.setViewportSize({width:900,height:760});await page.getByLabel('评论身份').scrollIntoViewIfNeeded();
  await page.locator('.community-comment-form').screenshot({path:join(output,'comment-compact.png')});
  assert.deepEqual(errors,[]);
  writeFileSync(join(output,'report.json'),JSON.stringify({passed:true,anonymousCode:true,nicknamePersistence:true,cancel:true,emptyRejected:true,enterDoesNotPost:true,postedName:true,failureRetainsDraft:true,idempotentRetry:true,errors},null,2));
  console.log('PASS comment identity, nickname save/cancel, posting, durable storage and idempotent retry.');
}finally{await browser.close();}
