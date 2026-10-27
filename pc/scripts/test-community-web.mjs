// Isolated real server UI + native mock bridge. Never writes production community or devices.
import assert from 'node:assert/strict';
import {mkdirSync,writeFileSync} from 'node:fs';
import {join} from 'node:path';
import {chromium} from 'playwright';
const output=process.env.PET_UI_REPORT_DIR;assert(output);mkdirSync(output,{recursive:true});
const browser=await chromium.launch({executablePath:'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try{
 const page=await browser.newPage({viewport:{width:1280,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.goto('http://127.0.0.1:4215/tests/component-center-review.html');
 async function detail(){await page.getByRole('tab',{name:'社区广场',exact:true}).click();const f=page.frameLocator('iframe[title="哈基米社区网页"]');await f.getByRole('button',{name:/社区回归样例/}).first().click();await f.getByRole('heading',{name:'评论',exact:true}).waitFor();return f;}
 let frame=await detail();const form=frame.locator('.comment-form');
 const code=await form.getByLabel('评论身份').innerText();assert.match(code,/^用户-[a-f0-9]{12}$/);
 assert.equal(await frame.getByText('公开显示名',{exact:true}).count(),0);assert.equal(await form.getByRole('checkbox').count(),0);
 await form.getByRole('button',{name:'编辑昵称'}).click();await form.getByLabel('昵称',{exact:true}).fill('取消昵称');await form.getByLabel('昵称',{exact:true}).press('Escape');assert.equal(await form.getByLabel('评论身份').innerText(),code);
 await form.getByRole('button',{name:'编辑昵称'}).click();await form.getByLabel('昵称',{exact:true}).fill('  ');await form.getByRole('button',{name:'保存',exact:true}).click();await form.getByRole('alert').filter({hasText:'请输入'}).waitFor();
 await form.getByLabel('昵称',{exact:true}).fill('橘子汽水');await form.getByLabel('昵称',{exact:true}).press('Enter');await form.getByLabel('评论身份').filter({hasText:'橘子汽水'}).waitFor();
 const calls=()=>page.evaluate(()=>window.__reviewCalls.filter(x=>x.command==='community_request').map(x=>x.args.input));
 assert.equal((await calls()).filter(x=>x.action==='comment').length,0);
 assert.equal(await form.getByRole('alert').textContent(),'','saved nickname clears validation errors');
 await form.getByLabel('评论内容').fill('<img src=x onerror=alert(1)> 很好玩');await form.getByRole('button',{name:'发表评论'}).click();await frame.locator('.comments article').first().waitFor();assert.equal(await frame.locator('.comments article img').count(),0);
 assert.equal((await calls()).find(x=>x.action==='comment').name,'橘子汽水');
 await page.evaluate(()=>window.__failCommentOnce=true);await form.getByLabel('评论内容').fill('模拟回执中断重试');await form.getByRole('button',{name:'发表评论'}).click();await form.getByRole('alert').filter({hasText:'模拟回执中断'}).waitFor();assert.equal(await form.getByLabel('评论内容').inputValue(),'模拟回执中断重试');
 await form.getByRole('button',{name:'发表评论'}).click();await frame.locator('.comments article').nth(1).waitFor();const sent=(await calls()).filter(x=>x.action==='comment');assert.equal(sent.at(-1).requestId,sent.at(-2).requestId);assert.equal(await frame.locator('.comments article').count(),2);
 await frame.getByRole('button',{name:'收藏',exact:true}).click();await frame.getByRole('button',{name:'已收藏',exact:true}).waitFor();
 await frame.getByRole('button',{name:'下载到我的组件',exact:true}).click();await page.getByRole('alertdialog',{name:'下载社区组件'}).waitFor();assert.equal((await calls()).filter(x=>x.action==='download').length,0);await page.getByRole('button',{name:'取消下载'}).click();
 await frame.getByRole('button',{name:'下载到我的组件',exact:true}).click();await page.getByRole('alertdialog',{name:'下载社区组件'}).waitFor();await page.getByRole('button',{name:'确认下载'}).click();await frame.getByRole('status').filter({hasText:'已校验'}).waitFor();assert.equal((await calls()).filter(x=>x.action==='download').length,1);
 await form.screenshot({path:join(output,'server-comment-nickname.png')});await page.screenshot({path:join(output,'client-server-detail.png'),fullPage:true});
 await page.reload();frame=await detail();assert.equal(await frame.getByLabel('评论身份').innerText(),'橘子汽水');
 await page.getByRole('tab',{name:'我的收藏',exact:true}).click();await page.frameLocator('iframe').getByRole('button',{name:/社区回归样例/}).first().waitFor();
 await page.setViewportSize({width:900,height:780});await page.screenshot({path:join(output,'client-server-favorites.png')});
 assert.deepEqual(errors,[]);writeFileSync(join(output,'browser-report.json'),JSON.stringify({passed:true,serverOwnedUi:true,anonymousIdentity:true,nicknamePersistence:true,plainTextComments:true,idempotentRetry:true,favoritesPersistence:true,nativeDownloadConfirmation:true,errors},null,2));console.log('PASS server UI + bridge + nickname + comments + favorites + native confirmation');
}finally{await browser.close();}
