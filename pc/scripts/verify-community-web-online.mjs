// Production acceptance is strictly read-only: never post likes, comments or download tickets.
import assert from 'node:assert/strict';
import {chromium} from 'playwright';
import {mkdirSync,writeFileSync} from 'node:fs';
import {join} from 'node:path';
const origin='https://115.191.67.48',output=process.env.PET_UI_REPORT_DIR;assert(output);mkdirSync(output,{recursive:true});
const browser=await chromium.launch({executablePath:'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try{
 const page=await browser.newPage({viewport:{width:1440,height:1000}}),errors=[];
 page.on('pageerror',e=>errors.push(e.message));
 await page.route('**/api/v1/**',route=>route.request().method()==='GET'?route.continue():route.abort());
 const response=await page.goto(`${origin}/community/`);assert.equal(response.status(),200);
 assert.equal(response.headers()['x-frame-options'],undefined);assert.match(response.headers()['content-security-policy'],/frame-ancestors/);
 await page.locator('.card').first().waitFor();await page.locator('.card img').evaluateAll(images=>Promise.all(images.map(img=>{img.loading='eager';return img.decode();})));
 await page.screenshot({path:join(output,'online-community.png'),fullPage:true});
 const name=await page.locator('.card h3').first().innerText();await page.locator('.card-open').first().click();await page.getByRole('heading',{name:'评论',exact:true}).waitFor();
 assert.equal(await page.getByRole('button',{name:'请在 Pet Manager 中下载'}).isDisabled(),true);
 await page.screenshot({path:join(output,'online-detail.png'),fullPage:true});
 await page.setViewportSize({width:480,height:850});await page.getByLabel('评论身份').scrollIntoViewIfNeeded();await page.screenshot({path:join(output,'online-detail-mobile.png'),fullPage:true});
 const admin=await page.request.get(`${origin}/admin/`);assert.equal(admin.status(),200);assert.match(admin.headers()['x-frame-options'],/DENY/);assert.match(await admin.text(),/登录/);
 const health=await page.request.get(`${origin}/api/v1/health`);assert.equal((await health.json()).ok,true);
 const mine=await page.request.get(`${origin}/api/v1/mine`);assert.equal(mine.status(),401);
 assert.deepEqual(errors,[]);
 writeFileSync(join(output,'online-report.json'),JSON.stringify({passed:true,httpsVerified:true,publicWeb:true,detail:name,adminRequiresLogin:true,adminFrameDenied:true,anonymousOwnerDenied:true,readOnly:true,errors},null,2));console.log('PASS online HTTPS, public gallery/detail, anonymous identity, responsive layout and admin isolation (read-only)');
}finally{await browser.close();}
