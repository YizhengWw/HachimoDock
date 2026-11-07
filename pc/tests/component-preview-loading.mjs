// Requires the local component-center-review fixture server; uses an isolated browser context.
import {chromium} from 'playwright';
import assert from 'node:assert/strict';
const browser=await chromium.launch({executablePath:process.env.PET_TEST_BROWSER_EXECUTABLE || undefined,headless:true});
let release;const gate=new Promise(resolve=>release=resolve);let requests=0;const errors=[];
try {
 const context=await browser.newContext({serviceWorkers:'block'});
 const page=await context.newPage();page.on('pageerror',e=>errors.push(e.message));
 await page.route('**/*',async route=>{
  const u=new URL(route.request().url());
  if(!['127.0.0.1','localhost'].includes(u.hostname))return route.abort();
  if(u.pathname.endsWith('/ComponentPreviewModal.jsx')){requests++;await gate;}
  return route.continue();
 });
 await page.goto('http://127.0.0.1:4191/tests/component-center-review.html');
 const card=page.getByRole('button',{name:'查看 回归测试组件 详情',exact:true});await card.waitFor();
 assert.equal(requests,0,'preview module must not load with the initial library');
 await card.click();await page.getByText('正在加载组件预览…',{exact:true}).waitFor();
 assert.equal(requests,1);
 await page.keyboard.press('Escape');
 await page.getByText('正在加载组件预览…',{exact:true}).waitFor({state:'hidden'});
 release();
 await card.click();
 const dialog=page.locator('[role="dialog"][aria-modal="true"]');await dialog.waitFor();
 await page.getByText('正在加载组件预览…',{exact:true}).waitFor({state:'hidden'});
 assert.ok((await dialog.innerText()).includes('回归测试组件'));
 await page.keyboard.press('Escape');await dialog.waitFor({state:'hidden'});
 const mutations=await page.evaluate(()=>window.__reviewCalls.filter(c=>['install_clawpkg_over_usb','delete_component_from_library','remove_widget_from_device'].includes(c.command)));
 assert.deepEqual(mutations,[]);assert.deepEqual(errors,[]);
 console.log('PASS: deferred preview import; loading/Escape cancellation; reopen/render; no device mutations or page errors.');
}finally{release();await browser.close();}
