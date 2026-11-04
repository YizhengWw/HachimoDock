// Browser acceptance of production React surfaces with synthetic contract data.
// Run a loopback-only Vite server separately; no Pet client/services are started.
import assert from 'node:assert/strict';
import {mkdirSync, writeFileSync} from 'node:fs';
import {resolve, join} from 'node:path';
import {chromium} from 'playwright';

const [output, label = 'run'] = process.argv.slice(2);
if (!output || !/^[\w-]+$/.test(label)) throw Error('Provide evidence directory and safe label');
const evidence = resolve(output); mkdirSync(evidence, {recursive: true});
const url = new URL(process.env.PET_FRAMEWORK_UI_URL || 'http://127.0.0.1:4198/tests/manual/framework-ui.html');
if (url.protocol !== 'http:' || url.hostname !== '127.0.0.1') throw Error('Only a loopback fixture is allowed');
const results = [], errors = [], blocked = [];
const browser = await chromium.launch({headless: true, ...(process.env.PET_TEST_BROWSER ? {executablePath: process.env.PET_TEST_BROWSER} : {})});
const context = await browser.newContext({viewport: {width: 1450, height: 1250}, serviceWorkers: 'block'});
await context.route('**/*', route => {
  const request = new URL(route.request().url());
  if (request.origin === url.origin || request.protocol === 'data:') return route.continue();
  blocked.push(`${request.protocol}//${request.hostname}`); return route.abort('blockedbyclient');
});
await context.routeWebSocket('**/*', route => {
  const request = new URL(route.url());
  if (request.hostname === '127.0.0.1' && request.port === url.port) route.connectToServer();
  else {blocked.push(`${request.protocol}//${request.hostname}`); route.close();}
});
const page = await context.newPage(); page.on('pageerror', error => errors.push(error.message));
page.setDefaultTimeout(5000);
const article = id => page.locator(`[data-fixture="${id}"]`);
const state = id => page.evaluate(id => window.__frameworkUi.drivers[id].state(), id);
const reset = async id => {
  await page.evaluate(id => window.__frameworkUi.drivers[id].reset(), id);
  await page.waitForFunction(id => window.__frameworkUi.drivers[id].state().vars[id === 'multi' ? 'hits' : 'choice'] === (id === 'multi' ? 0 : 10), id);
  await page.waitForTimeout(30);
};
const action = async (id, name) => {
  await article(id).locator(`.physical button[data-action="${name}"]`).click();
  await page.waitForTimeout(20);
};
async function point(id, x, y) {
  await article(id).locator('svg').scrollIntoViewIfNeeded();
  const box = await article(id).locator('svg').boundingBox();
  return {x: box.x + x * box.width / 640, y: box.y + y * box.height / 480};
}
async function click(id, x, y) {const q = await point(id, x, y); await page.mouse.click(q.x, q.y); await page.waitForTimeout(20);}
async function test(name, run) {await run(); results.push({name, passed: true}); console.log(`PASS ${name}`);}
async function previewValues() {
  return article('preview').locator('svg').evaluate(svg => {
    const text = (x, y) => svg.querySelector(`text[x="${x}"][y="${y}"]`)?.textContent;
    return {mode: text(24, 142), amount: text(24, 238), savedMode: text(336, 142), savedAmount: text(456, 142), focus: text(336, 238), editing: text(456, 238)};
  });
}
async function previewButton(name) {
  await article('preview').getByRole('group', {name: '表单实体按键预览'}).getByRole('button', {name, exact: true}).click();
  await page.waitForTimeout(20);
}
try {
  const response=await page.goto(url.href);assert.equal(response?.status(),200,'fixture must be served from the PC test root');
  await page.waitForFunction(() => window.__frameworkUi?.ready && window.__frameworkUi.drivers.multi);
  assert.equal(await page.evaluate(() => !!window.__TAURI_INTERNALS__), false);
  await test('physical keys and pointer confirmations share admission; changed host inputs or geometry cancel only dependent operations',async()=>{
    const panel=article('guards'),controls=panel.getByRole('group',{name:'程序组件实体按键预览'});
    const count=()=>panel.locator('svg text[x="20"][y="30"]').textContent();
    const reply=(fresh,step)=>({fresh,variables:{step},ui:{mode:0,context:'same',actions:['host.increment']}});
    await controls.getByRole('button',{name:'Add',exact:true}).click();assert.equal(await panel.getByRole('dialog').count(),1);assert.equal(await count(),'0');
    await controls.getByRole('button',{name:'Add',exact:true}).click();
    await page.waitForFunction(()=>document.querySelector('[data-fixture="guards"] svg text[x="20"][y="30"]')?.textContent==='2');
    await controls.getByRole('button',{name:'Add',exact:true}).click();
    await page.evaluate(r=>window.__frameworkUi.drivers.guards.replace(r),reply(false,2));
    await page.waitForFunction(()=>!document.querySelector('[data-fixture="guards"] [role="dialog"]'));assert.equal(await count(),'2');
    await controls.getByRole('button',{name:'Subtract',exact:true}).click();assert.equal(await panel.getByRole('dialog').count(),1);
    await page.evaluate(r=>window.__frameworkUi.drivers.guards.replace(r),reply(true,4));await page.waitForTimeout(50);
    assert.equal(await panel.getByRole('dialog').count(),1);await controls.getByRole('button',{name:'Add',exact:true}).click();
    await page.waitForFunction(()=>document.querySelector('[data-fixture="guards"] svg text[x="20"][y="30"]')?.textContent==='1');
    await click('guards',100,364);assert.equal(await panel.getByRole('dialog').count(),1);
    await page.evaluate(r=>window.__frameworkUi.drivers.guards.replace(r),reply(true,5));
    await page.waitForFunction(()=>!document.querySelector('[data-fixture="guards"] [role="dialog"]'));assert.equal(await count(),'1');
    await page.evaluate(r=>window.__frameworkUi.drivers.guards.replace(r),reply(false,5));
    await page.waitForFunction(()=>document.querySelector('[data-fixture="guards"] svg [aria-label="Guarded subtract"]')?.getAttribute('aria-disabled')==='true');
    assert.equal(await controls.getByRole('button',{name:'Subtract',exact:true}).isEnabled(),true);
  });
  await test('actual C program admission starts one shared key/touch cooldown and permits a later operation',async()=>{
    const panel=article('cooldown'),controls=panel.getByRole('group',{name:'程序组件实体按键预览'}),add=controls.getByRole('button',{name:'Add',exact:true});
    await add.click();await page.waitForFunction(()=>document.querySelector('[data-fixture="cooldown"] svg text[x="20"][y="30"]')?.textContent==='2');
    assert.equal(await add.isDisabled(),true);
    await page.evaluate(()=>{const add=[...document.querySelectorAll('[data-fixture="cooldown"] [role="group"] button')].find(b=>b.textContent==='Add');add.click();});
    await page.waitForTimeout(60);assert.equal(await panel.locator('svg text[x="20"][y="30"]').textContent(),'2');
    await page.waitForFunction(()=>[...document.querySelectorAll('[data-fixture="cooldown"] [role="group"] button')].some(b=>b.textContent==='Add'&&!b.disabled));
    await add.click();await page.waitForFunction(()=>document.querySelector('[data-fixture="cooldown"] svg text[x="20"][y="30"]')?.textContent==='4');
  });
  await test('host replies preserve one native program instance, capture inputs once and do not block independent native actions',async()=>{
    const panel=article('owners'),controls=panel.getByRole('group',{name:'程序组件实体按键预览'});
    const count=()=>panel.locator('svg text[x="20"][y="30"]').textContent();
    assert.equal(await count(),'0');
    await controls.getByRole('button',{name:'Add',exact:true}).click();
    await page.waitForFunction(()=>document.querySelector('[data-fixture="owners"] svg text[x="20"][y="30"]')?.textContent==='5');
    await page.evaluate(()=>window.__frameworkUi.drivers.owners.replace({fresh:true,variables:{step:9,count:800},ui:{mode:0,context:'b',actions:['host.increment']}}));
    assert.equal(await count(),'5');
    await controls.getByRole('button',{name:'Add',exact:true}).click();
    await page.waitForFunction(()=>document.querySelector('[data-fixture="owners"] svg text[x="20"][y="30"]')?.textContent==='14');
    await page.evaluate(()=>window.__frameworkUi.drivers.owners.replace({fresh:false}));
    assert.equal(await controls.getByRole('button',{name:'Add',exact:true}).isDisabled(),true);
    assert.equal(await controls.getByRole('button',{name:'Subtract',exact:true}).isEnabled(),true);
    await controls.getByRole('button',{name:'Subtract',exact:true}).click();
    await page.waitForFunction(()=>document.querySelector('[data-fixture="owners"] svg text[x="20"][y="30"]')?.textContent==='13');
    assert.deepEqual(await page.evaluate(()=>window.__frameworkUi.drivers.owners.received()),[]);
    await page.evaluate(()=>window.__frameworkUi.drivers.owners.replace({fresh:true,variables:{step:4},ui:{mode:0,context:'c',actions:['host.increment']}}));
    await click('owners',350,264);assert.deepEqual(await page.evaluate(()=>window.__frameworkUi.drivers.owners.received()),['host.increment']);
  });
  await test('production component preview executes declared events through the same C interpreter',async()=>{
    const controls=article('componentProgram').getByRole('group',{name:'程序组件实体按键预览'});
    await controls.getByRole('button',{name:'Add',exact:true}).click();
    await page.waitForFunction(()=>[...document.querySelectorAll('[data-fixture="componentProgram"] svg text')].some(n=>n.textContent==='2'));
    await controls.getByRole('button',{name:'Subtract',exact:true}).click();
    await page.waitForFunction(()=>[...document.querySelectorAll('[data-fixture="componentProgram"] svg text')].some(n=>n.textContent==='1'));
  });
  await test('production service panel retains native form drafts and program commits across actual polling and host commands',async()=>{
    const panel=article('ownedPanel'),controls=panel.getByRole('group',{name:'程序组件实体按键预览'});
    const count=()=>panel.locator('svg text[x="260"][y="70"]').textContent();
    await controls.getByRole('button',{name:'Edit / accept',exact:true}).click();
    const input=panel.getByRole('textbox',{name:'文本草稿',exact:true});await input.fill('春天');
    await click('ownedPanel',100,224);await page.waitForFunction(()=>document.querySelector('[data-fixture="ownedPanel"] svg text[x="260"][y="70"]')?.textContent==='5');
    await page.evaluate(()=>window.__frameworkUi.drivers.ownedPanel.setStep(7));await page.waitForTimeout(2200);
    assert.equal(await count(),'5');assert.equal(await input.inputValue(),'春天');
    await click('ownedPanel',100,224);await page.waitForFunction(()=>document.querySelector('[data-fixture="ownedPanel"] svg text[x="260"][y="70"]')?.textContent==='12');
    await panel.locator('.component-app__actions button').click();await page.waitForTimeout(60);
    assert.equal(await count(),'12');assert.equal(await input.inputValue(),'春天');
    const requests=await page.evaluate(()=>window.__frameworkUi.drivers.ownedPanel.requests());
    const actions=requests.filter(r=>r.operation==='action');assert.equal(actions.length,1);assert.equal(actions[0].action,'host.increment');assert.equal(typeof actions[0].eventId,'string');
    await panel.getByRole('button',{name:'停用服务（保留本机记录）',exact:true}).click();
    await click('ownedPanel',300,224);await page.waitForFunction(()=>document.querySelector('[data-fixture="ownedPanel"] svg text[x="260"][y="70"]')?.textContent==='11');
    assert.equal(await input.inputValue(),'春天');
  });
  await test('native pointer ignores unrelated host epochs while host-read program gestures reject changed and away-back publications',async()=>{
    const panel=article('owners'),value=()=>panel.locator('svg text[x="20"][y="30"]').textContent();
    const reply=step=>({fresh:true,variables:{step},ui:{mode:0,context:'same-context',actions:['host.increment']}});
    let q=await point('owners',360,148);await page.mouse.move(q.x,q.y);await page.mouse.down();
    await page.evaluate(r=>window.__frameworkUi.drivers.owners.replace(r),reply(6));await page.waitForTimeout(30);await page.mouse.up();
    await page.waitForFunction(()=>document.querySelector('[data-fixture="owners"] svg text[x="20"][y="30"]')?.textContent==='12');
    q=await point('owners',100,148);await page.mouse.move(q.x,q.y);await page.mouse.down();
    await page.evaluate(r=>window.__frameworkUi.drivers.owners.replace(r),reply(7));await page.waitForTimeout(30);await page.mouse.up();await page.waitForTimeout(100);
    assert.equal(await value(),'12');
    await page.mouse.down();await page.evaluate(r=>window.__frameworkUi.drivers.owners.replace(r),reply(7));await page.waitForTimeout(30);await page.mouse.up();
    await page.waitForFunction(()=>document.querySelector('[data-fixture="owners"] svg text[x="20"][y="30"]')?.textContent==='19');
    await page.mouse.down();await page.evaluate(replies=>window.__frameworkUi.drivers.owners.replaceMany(replies),[reply(8),reply(7)]);await page.waitForTimeout(30);await page.mouse.up();await page.waitForTimeout(100);
    assert.equal(await value(),'19');
  });
  await test('component hide cancels queued work, preserves committed cells and creates a fresh explicit owner on reopen',async()=>{
    await page.evaluate(()=>{
      const panel=document.querySelector('[data-fixture="componentProgram"]');
      [...panel.querySelectorAll('[aria-label="程序组件实体按键预览"] button')].find(b=>b.textContent==='Add').click();
      window.__frameworkUi.setComponentProgramActive(false);
    });
    await page.waitForTimeout(120);
    assert.equal(await article('componentProgram').locator('svg text').filter({hasText:/^1$/}).count(),1);
    await page.evaluate(()=>window.__frameworkUi.setComponentProgramActive(true));
    await article('componentProgram').getByRole('group',{name:'程序组件实体按键预览'}).getByRole('button',{name:'Add',exact:true}).click();
    await page.waitForFunction(()=>[...document.querySelectorAll('[data-fixture="componentProgram"] svg text')].some(n=>n.textContent==='3'));
  });
  await test('a trapped component preserves its last visible model and exposes a stopped state without automatic retries',async()=>{
    const controls=article('componentProgram').getByRole('group',{name:'程序组件实体按键预览'});
    await controls.getByRole('button',{name:'Exceed',exact:true}).click();
    await page.waitForFunction(()=>document.querySelector('[data-fixture="componentProgram"] [data-component-program-phase]')?.dataset.componentProgramPhase==='stopped');
    assert.equal(await article('componentProgram').locator('svg text').filter({hasText:/^3$/}).count(),1);
    assert.equal(await controls.getByRole('button',{name:'Add',exact:true}).isDisabled(),true);
    await page.waitForTimeout(200);assert.equal(await article('componentProgram').locator('svg text').filter({hasText:/^3$/}).count(),1);
    await article('componentProgram').screenshot({path:join(evidence,`${label}-component-program.png`)});
  });
  await test('full program queue visibly disables input and drains exactly the already accepted actions',async()=>{
    await page.evaluate(()=>window.__frameworkUi.setComponentProgramActive(false));await page.waitForTimeout(30);
    await page.evaluate(()=>window.__frameworkUi.setComponentProgramActive(true));
    const controls=article('componentProgram').getByRole('group',{name:'程序组件实体按键预览'});
    await controls.getByRole('button',{name:'Add',exact:true}).waitFor();
    await page.waitForFunction(()=>[...document.querySelectorAll('[data-fixture="componentProgram"] [aria-label="程序组件实体按键预览"] button')].some(b=>b.textContent==='Add'&&!b.disabled));
    const full=await page.evaluate(()=>new Promise((resolve,reject)=>{
      const panel=document.querySelector('[data-fixture="componentProgram"]');
      const add=[...panel.querySelectorAll('[aria-label="程序组件实体按键预览"] button')].find(b=>b.textContent==='Add');
      const timeout=setTimeout(()=>{observer.disconnect();reject(Error('queue backpressure was not visible'));},1500);
      const observer=new MutationObserver(()=>{if(add.disabled&&panel.textContent.includes('程序队列已满')){clearTimeout(timeout);observer.disconnect();resolve(true);}});
      observer.observe(panel,{subtree:true,childList:true,attributes:true});for(let i=0;i<4;i++)add.click();
    }));assert.equal(full,true);
    await page.waitForFunction(()=>[...document.querySelectorAll('[data-fixture="componentProgram"] svg text')].some(n=>n.textContent==='11'));
    assert.equal(await controls.getByRole('button',{name:'Add',exact:true}).isDisabled(),false);
  });
  await test('host collection renders visible records as real standard nodes and dispatches the clicked row',async()=>{
    assert.equal(await article('hostList').locator('svg').getByRole('button',{name:'第一项',exact:true}).count(),1);
    assert.equal(await article('hostList').locator('svg').getByRole('button',{name:'第二项',exact:true}).count(),1);
    await click('hostList',100,125);assert.deepEqual(await page.evaluate(()=>window.__frameworkUi.drivers.hostList.received()),['host.pick_a']);
    await article('hostList').screenshot({path:join(evidence,`${label}-host-list.png`)});
  });
  await test('production mixed program preview preserves an edited draft across native events and cancels it only on navigation',async()=>{
    const panel=article('mixedProgram'),controls=panel.getByRole('group',{name:'程序组件实体按键预览'});
    await page.waitForFunction(()=>[...document.querySelectorAll('[data-fixture="mixedProgram"] svg text')].some(n=>n.textContent==='1'));
    await controls.getByRole('button',{name:'Edit / accept',exact:true}).click();
    const input=panel.getByRole('textbox',{name:'文本草稿',exact:true});await input.fill('春天');
    await click('mixedProgram',100,224);
    await page.waitForFunction(()=>[...document.querySelectorAll('[data-fixture="mixedProgram"] svg text')].some(n=>n.textContent==='3'));
    assert.equal(await input.inputValue(),'春天');
    await controls.getByRole('button',{name:'Edit / accept',exact:true}).click();assert.equal(await input.count(),0);
    await controls.getByRole('button',{name:'Edit / accept',exact:true}).click();await input.fill('暂存');
    await click('mixedProgram',100,144);assert.equal(await input.count(),0);
    await click('mixedProgram',300,144);
    await controls.getByRole('button',{name:'Edit / accept',exact:true}).click();assert.equal(await input.inputValue(),'春天');
    assert.equal(await controls.getByRole('button',{name:'Previous',exact:true}).isDisabled(),true);
    assert.equal(await controls.getByRole('button',{name:'Next',exact:true}).isDisabled(),true);
    await controls.getByRole('button',{name:'Cancel',exact:true}).click();
    await panel.screenshot({path:join(evidence,`${label}-mixed-program.png`)});
  });
  await test('foreground program blur freezes ordinary ticks and input until an explicit fresh activation',async()=>{
    const panel=article('lifecycleProgram'),pulse=()=>panel.locator('svg text[x="20"][y="320"]').textContent();
    await page.waitForFunction(()=>Number(document.querySelector('[data-fixture="lifecycleProgram"] svg text[x="20"][y="320"]')?.textContent)>0);
    const a=await pulse();await page.waitForTimeout(150);assert.notEqual(await pulse(),a);
    await page.evaluate(()=>window.dispatchEvent(new Event('blur')));
    const frozen=await pulse();await page.waitForTimeout(220);assert.equal(await pulse(),frozen);
    assert.equal(await panel.getByRole('group',{name:'程序组件实体按键预览'}).getByRole('button',{name:'Add',exact:true}).isDisabled(),true);
    await page.evaluate(()=>window.dispatchEvent(new Event('focus')));await page.waitForTimeout(150);assert.equal(await pulse(),frozen);
    await page.evaluate(()=>window.__frameworkUi.setLifecycleActive(false));await page.waitForTimeout(30);
    await page.evaluate(()=>window.__frameworkUi.setLifecycleActive(true));await page.waitForTimeout(180);
    assert(Number(await pulse())>Number(frozen));
  });
  await test('unchanged host poll preserves a press but reordered and away-back replies cancel it',async()=>{
    const original=(await state('hostList')).collectionView;
    await page.evaluate(()=>window.__frameworkUi.drivers.hostList.clear());
    let q=await point('hostList',100,125);await page.mouse.move(q.x,q.y);await page.mouse.down();
    await page.evaluate(model=>window.__frameworkUi.drivers.hostList.replace(model),original);await page.waitForTimeout(30);await page.mouse.up();
    assert.deepEqual(await page.evaluate(()=>window.__frameworkUi.drivers.hostList.received()),['host.pick_a']);
    const changed=structuredClone(original);changed.rows[0].label='Replacement';
    await page.evaluate(()=>window.__frameworkUi.drivers.hostList.clear());
    q=await point('hostList',100,125);await page.mouse.move(q.x,q.y);await page.mouse.down();
    await page.evaluate(models=>window.__frameworkUi.drivers.hostList.replaceMany(models),[changed,original]);
    await page.waitForTimeout(30);await page.mouse.up();
    assert.deepEqual(await page.evaluate(()=>window.__frameworkUi.drivers.hostList.received()),[]);
    assert.equal((await state('hostList'))._collectionEpoch,3);
    await click('hostList',100,125);assert.deepEqual(await page.evaluate(()=>window.__frameworkUi.drivers.hostList.received()),['host.pick_a']);
  });
  await test('empty missing and incompatible host row data hide targets rather than reusing old labels',async()=>{
    const original=(await state('hostList')).collectionView;
    const empty=structuredClone(original);empty.count=0;empty.selectionKey='offpage';empty.rows=empty.rows.map(()=>({present:false,key:'',label:'',detail:'',index:-1,selected:false,truncated:false}));
    for(const model of [empty,null,{...original,source:'other'}]){
      await page.evaluate(v=>window.__frameworkUi.drivers.hostList.replace(v),model);await page.waitForTimeout(30);
      assert.equal(await article('hostList').locator('svg [role="button"]').count(),0);
      assert.equal(await article('hostList').locator('svg text').filter({hasText:new RegExp(`^${model===empty?'暂无条目':'等待电脑数据'}$`)}).count(),1);
    }
    await page.evaluate(v=>window.__frameworkUi.drivers.hostList.replace(v),original);await page.waitForTimeout(30);
    assert.equal(await article('hostList').locator('svg [role="button"]').count(),2);
  });
  await test('browser authoring runs one shared C program slice per click and publishes only a committed model',async()=>{
    const panel=article('program');await panel.locator('[data-program-enqueue]').click();
    assert.equal((await state('program')).snapshot.revision,0);
    await panel.locator('[data-program-pump]').click(); /* begin, no instructions */
    assert.equal((await state('program')).phase,'yielded');assert.deepEqual((await state('program')).snapshot.values,[0]);
    for(let i=0;i<10&&(await state('program')).phase!=='committed';i++)await panel.locator('[data-program-pump]').click();
    assert.equal((await state('program')).phase,'committed');assert.equal((await state('program')).snapshot.revision,1);assert.deepEqual((await state('program')).snapshot.values,[5]);
    await panel.locator('[data-program-enqueue]').click();await panel.locator('[data-program-pump]').click();await panel.locator('[data-program-cancel]').click();
    await panel.locator('[data-program-pump]').click();assert.equal((await state('program')).phase,'stopped');assert.deepEqual((await state('program')).snapshot.values,[5]);
  });
  await test('hiding a program authoring view revokes the old runtime, without resuming it on explicit fresh remount',async()=>{
    await page.evaluate(()=>{window.__oldProgram=window.__frameworkUi.drivers.program;});
    await page.locator('[data-program-visibility]').click();assert.equal(await article('program').count(),0);
    assert.equal(await page.evaluate(()=>window.__oldProgram.pump()),'stopped');
    await page.locator('[data-program-visibility]').click();await page.waitForFunction(()=>window.__frameworkUi.drivers.program);
    assert.deepEqual((await state('program')).snapshot.values,[0]);assert.equal((await state('program')).snapshot.revision,0);
    assert.equal(await page.evaluate(()=>window.__oldProgram.pump()),'stopped');await page.evaluate(()=>delete window.__oldProgram);
    await article('program').screenshot({path:join(evidence,`${label}-program.png`)});
  });
  await test('production ComponentAppPreview edits drafts and cancels without saving', async () => {
    assert.deepEqual(await previewValues(), {mode: 'Quiet', amount: '2', savedMode: '10', savedAmount: '2', focus: '0', editing: '0'});
    await previewButton('Edit / accept'); await previewButton('Increase');
    assert.equal((await previewValues()).mode, 'Balanced'); assert.equal((await previewValues()).savedMode, '10');
    await previewButton('Cancel'); assert.equal((await previewValues()).mode, 'Quiet'); assert.equal((await previewValues()).editing, '0');
  });
  await test('one form accept commits both fields atomically in real React state', async () => {
    for (const a of ['form.accept', 'value.up', 'field.next', 'value.up']) await action('form', a);
    assert.deepEqual([(await state('form')).vars.choice, (await state('form')).vars.amount], [10, 2]);
    await action('form', 'form.accept');
    const final = await state('form'); assert.deepEqual([final.vars.choice, final.vars.amount, final.vars.editing], [20, 4, 0]);
    const pairs = await page.evaluate(() => window.__frameworkUi.drivers.form.observations().map(s => [s.vars.choice, s.vars.amount]));
    assert(pairs.every(pair => JSON.stringify(pair) === '[10,2]' || JSON.stringify(pair) === '[20,4]'), 'no half-committed render');
  });
  await test('root back is visibly disabled and cannot discard an open draft', async () => {
    await reset('form'); await action('form', 'form.accept'); await action('form', 'value.up');
    assert.equal(await article('form').locator('svg').getByRole('button', {name: 'Back', exact: true}).getAttribute('aria-disabled'), 'true');
    await click('form', 500, 430);
    const s = await state('form'); assert.equal(s.page, 'edit'); assert.equal(s.vars.choiceDraft, 20); assert.equal(s.vars.editing, 1);
  });
  await test('page navigation discards drafts and disables form actions off-page', async () => {
    await click('form', 500, 345); const s = await state('form');
    assert.equal(s.page, 'home'); assert.deepEqual([s.vars.choice, s.vars.choiceDraft, s.vars.editing], [10, 10, 0]);
    await click('form', 100, 345); assert.equal((await state('form')).vars.editing, 0);
    await click('form', 500, 430); assert.equal((await state('form')).page, 'edit');
  });
  await test('production preview hide/blur discards drafts without changing committed fields', async () => {
    await previewButton('Edit / accept'); await previewButton('Increase');
    await page.evaluate(() => window.__frameworkUi.setPreviewActive(false)); await page.waitForTimeout(40);
    assert.equal((await previewValues()).mode, 'Quiet'); assert.equal((await previewValues()).editing, '0');
    await page.evaluate(() => window.__frameworkUi.setPreviewActive(true)); await page.waitForTimeout(30);
    await previewButton('Edit / accept'); await previewButton('Increase');
    await page.evaluate(() => window.dispatchEvent(new Event('blur'))); await page.waitForTimeout(30);
    assert.equal((await previewValues()).mode, 'Quiet'); assert.equal((await previewValues()).savedMode, '10');
  });
  await test('independent viewport clipping controls both drawing and clicks with bitmap textures', async () => {
    await reset('multi'); assert.equal(await article('multi').locator('svg image').count(), 2);
    await click('multi', 40, 390); assert.equal((await state('multi')).vars.hits, 0, 'clipped tail is not clickable');
    await click('multi', 40, 375); assert.equal((await state('multi')).vars.hits, 1);
    const pixels = await article('multi').locator('svg').evaluate(async svg => {
      const image = new Image(); image.src = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(new XMLSerializer().serializeToString(svg))}`;
      await image.decode(); const canvas = document.createElement('canvas'); canvas.width = 640; canvas.height = 480;
      const ctx = canvas.getContext('2d'); ctx.drawImage(image, 0, 0, 640, 480);
      return [40, 375, 40, 390, 320, 390, 40, 90].reduce((all, _, index, coords) => index % 2 ? all : [...all, [...ctx.getImageData(coords[index], coords[index + 1], 1, 1).data]], []);
    });
    assert.notDeepEqual(pixels[0], pixels[1], 'visible part is painted'); assert.deepEqual(pixels[1], pixels[2], 'tail is actually clipped'); assert.notDeepEqual(pixels[3], pixels[2], 'texture pixels exist');
    const rightY = await article('multi').locator('image[x="344"]').getAttribute('y');
    await action('multi', 'left.down'); assert.equal((await state('multi')).vars.right_offset, 0);
    assert.equal(await article('multi').locator('image[x="344"]').getAttribute('y'), rightY);
    await click('multi', 40, 250); assert.equal((await state('multi')).vars.hits, 2);
    await action('multi', 'right.down'); await click('multi', 360, 350); assert.equal((await state('multi')).vars.hits, 12);
    await article('multi').screenshot({path: join(evidence, `${label}-viewports.png`)});
  });
  await test('offset change between pointer down/up cancels the original click', async () => {
    await reset('multi'); const q = await point('multi', 40, 375); await page.mouse.move(q.x, q.y); await page.mouse.down();
    await page.evaluate(() => window.__frameworkUi.drivers.multi.action('left.down')); await page.waitForTimeout(30); await page.mouse.up();
    assert.equal((await state('multi')).vars.hits, 0);
  });
  await test('same-frame offset away/back also cancels the original click', async () => {
    await reset('multi'); const q = await point('multi', 40, 375); await page.mouse.move(q.x, q.y); await page.mouse.down();
    await page.evaluate(() => window.__frameworkUi.drivers.multi.actions(['left.down', 'left.up'])); await page.waitForTimeout(30); await page.mouse.up();
    const s = await state('multi'); assert.equal(s.vars.left_offset, 0); assert.equal(s.vars.hits, 0);
  });
  await test('wrong-page actions disabled and page away/back cancels pending click', async () => {
    await reset('multi'); await click('multi', 80, 436); assert.equal((await state('multi')).vars.hits, 0);
    const q = await point('multi', 40, 375); await page.mouse.move(q.x, q.y); await page.mouse.down();
    await page.evaluate(() => window.__frameworkUi.drivers.multi.actions(['open', 'back'])); await page.waitForTimeout(30); await page.mouse.up();
    assert.equal((await state('multi')).vars.hits, 0);
    await action('multi', 'open'); await click('multi', 40, 375); assert.equal((await state('multi')).vars.hits, 0);
    assert.equal(await article('multi').locator('svg').getByRole('button', {name: 'Left', exact: true}).getAttribute('aria-disabled'), 'true');
    await click('multi', 80, 436); assert.equal((await state('multi')).vars.hits, 1000);
  });
  await test('preview without sprite resources accepts a click spanning several clock ticks', async () => {
    const q = await point('plain', 100, 345); await page.mouse.move(q.x, q.y); await page.mouse.down();
    await page.waitForTimeout(250); await page.mouse.up();
    await page.waitForTimeout(30);
    assert.equal(await article('plain').locator('text[x="456"][y="238"]').textContent(), '1', 'clock-only renders must not cancel a pending click');
  });
  await test('fixed host collection rows cancel old pointer identity after data replacement',async()=>{
    await page.waitForFunction(()=>window.__frameworkUi.drivers.collection);
    const q=await point('collection',80,190);await page.mouse.move(q.x,q.y);await page.mouse.down();
    await page.evaluate(()=>window.__frameworkUi.drivers.collection.replace({
      vars:{rowa:'C',rowb:'A',chosen:'B',count:3},
      ui:{mode:0,context:'reordered-records',actions:['host.row_a','host.row_b','host.next']}}));
    await page.waitForTimeout(30);await page.mouse.up();
    assert.deepEqual(await page.evaluate(()=>window.__frameworkUi.drivers.collection.received()),[]);
    await click('collection',80,120);
    assert.deepEqual(await page.evaluate(()=>window.__frameworkUi.drivers.collection.received()),['host.row_a']);
    assert.equal((await state('collection')).vars.chosen,'B');
  });
  await test('empty collection slots are disabled and cannot select a different record',async()=>{
    await page.evaluate(()=>{window.__frameworkUi.drivers.collection.clear();window.__frameworkUi.drivers.collection.replace({
      vars:{rowa:'B',rowb:'',chosen:'B',count:3},
      ui:{mode:0,context:'last-page',actions:['host.row_a','host.previous','host.clear']}});});
    await page.waitForTimeout(30);
    assert.equal(await article('collection').locator('svg [role="button"]').nth(1).getAttribute('aria-disabled'),'true');
    await click('collection',80,190);
    assert.deepEqual(await page.evaluate(()=>window.__frameworkUi.drivers.collection.received()),[]);
    await article('collection').screenshot({path:join(evidence,`${label}-collection.png`)});
  });
  await test('shared native collection projects stable selection into real bounded AppSurface labels',async()=>{
    await page.waitForFunction(()=>window.__frameworkUi.drivers.nativeCollection);
    await click('nativeCollection',80,190);assert.equal((await state('nativeCollection')).saved.selected,'b');
    assert.equal((await state('nativeCollection')).vars.chosen,'Beta');
    await page.evaluate(()=>window.__frameworkUi.drivers.nativeCollection.replace([
      {key:'c',label:'Gamma'},{key:'a',label:'Alpha'},{key:'b',label:'Beta'}]));
    await page.waitForTimeout(30);assert.equal((await state('nativeCollection')).collection.selectedIndex,2);
    assert.equal((await state('nativeCollection')).vars.rowa,'Gamma');assert.equal((await state('nativeCollection')).vars.chosen,'Beta');
    const q=await point('nativeCollection',80,120);await page.mouse.move(q.x,q.y);await page.mouse.down();
    await page.evaluate(()=>window.__frameworkUi.drivers.nativeCollection.replace([
      {key:'a',label:'Alpha'},{key:'b',label:'Beta'},{key:'c',label:'Gamma'}]));
    await page.waitForTimeout(30);await page.mouse.up();assert.equal((await state('nativeCollection')).saved.selected,'b');
    await click('nativeCollection',80,120);assert.equal((await state('nativeCollection')).saved.selected,'a');
    assert.equal((await state('nativeCollection')).vars.chosen,'Alpha');
  });
  await test('shared collection empty slots and filtered selection preserve state without stale row actions',async()=>{
    await action('nativeCollection','host.next');assert.equal((await state('nativeCollection')).collection.offset,2);
    assert.equal(await article('nativeCollection').locator('svg [role="button"]').nth(1).getAttribute('aria-disabled'),'true');
    await click('nativeCollection',80,190);assert.equal((await state('nativeCollection')).saved.selected,'a');
    await page.evaluate(()=>window.__frameworkUi.drivers.nativeCollection.replace([]));await page.waitForTimeout(30);
    assert.equal((await state('nativeCollection')).vars.chosen,'');assert.equal((await state('nativeCollection')).saved.selected,'a');
    assert.equal((await state('nativeCollection')).collection.pages,0);assert.equal((await state('nativeCollection')).collection.canDown,false);
    await page.evaluate(()=>window.__frameworkUi.drivers.nativeCollection.replace([
      {key:'a',label:'Alpha'},{key:'b',label:'Beta'},{key:'c',label:'Gamma'}]));await page.waitForTimeout(30);
    assert.equal((await state('nativeCollection')).vars.chosen,'Alpha');
    await article('nativeCollection').screenshot({path:join(evidence,`${label}-native-collection.png`)});
  });
  await test('shared C canvas raster is visible and reacts to real runtime variable mutations',async()=>{
    await page.waitForFunction(()=>document.querySelector('[data-fixture="canvas"] [data-canvas-state="ready"]'));
    async function sample(){return article('canvas').locator('svg').evaluate(async svg=>{
      const image=new Image();image.src=`data:image/svg+xml;charset=utf-8,${encodeURIComponent(new XMLSerializer().serializeToString(svg))}`;
      await image.decode();const canvas=document.createElement('canvas');canvas.width=640;canvas.height=480;
      const ctx=canvas.getContext('2d');ctx.drawImage(image,0,0,640,480);
      return [[52,108],[80,128],[110,158]].map(([x,y])=>[...ctx.getImageData(x,y,1,1).data]);
    });}
    const before=await sample();assert.deepEqual(before[0],[255,0,0,255]);
    await action('canvas','move');const after=await sample();
    assert.deepEqual(after[0],[0,255,0,255]);assert.deepEqual(after[1],[255,0,0,255]);assert.deepEqual(before[2],after[2]);
    assert.equal(await article('canvas').locator('[data-canvas-error]').count(),0);
    await article('canvas').screenshot({path:join(evidence,`${label}-canvas.png`)});
  });
  await test('page-specific canvas disappears and returns without stale ownership or lost data',async()=>{
    const drawn=await article('canvas').locator('[data-canvas-state] rect').count();assert(drawn>0);
    await action('canvas','open');assert.equal(await article('canvas').locator('[data-canvas-state] rect').count(),0);
    await action('canvas','back');assert.equal(await article('canvas').locator('[data-canvas-state] rect').count(),drawn);
    assert.equal((await state('canvas')).vars.x,40);
  });
  await test('unavailable or mistyped canvas snapshots hide geometry instead of displaying invented zero values',async()=>{
    const saved=(await state('canvas')).vars;
    await page.evaluate(()=>window.__frameworkUi.drivers.canvas.replace({vars:{x:1,y:2}}));await page.waitForTimeout(30);
    assert.equal(await article('canvas').locator('[data-canvas-error]').getAttribute('data-canvas-error'),'canvas_snapshot_unavailable');
    assert.equal(await article('canvas').locator('[data-canvas-state] rect').count(),0);
    await page.evaluate(vars=>window.__frameworkUi.drivers.canvas.replace({vars}),{...saved,r:'bad'});await page.waitForTimeout(30);
    assert.equal(await article('canvas').locator('[data-canvas-error]').getAttribute('data-canvas-error'),'canvas_snapshot_invalid');
    await page.evaluate(vars=>window.__frameworkUi.drivers.canvas.replace({vars}),saved);await page.waitForTimeout(30);
    assert.equal(await article('canvas').locator('[data-canvas-error]').count(),0);assert((await article('canvas').locator('[data-canvas-state] rect').count())>0);
  });
  await test('mixed native text form accepts OS text into drafts and commits mixed fields atomically',async()=>{
    assert.equal(await article('mixed').getByRole('textbox',{name:'文本草稿',exact:true}).count(),0);
    await action('mixed','form.accept');const input=article('mixed').getByRole('textbox',{name:'文本草稿',exact:true});
    await input.fill('用户😀');assert.equal((await state('mixed')).vars.title,'中');assert.equal((await state('mixed')).vars.titleDraft,'用户😀');
    await action('mixed','field.next');await action('mixed','value.up');assert.equal((await state('mixed')).vars.amount,2);
    await action('mixed','form.accept');const s=await state('mixed');assert.deepEqual([s.vars.title,s.vars.amount,s.vars.editing],['用户😀',4,0]);
  });
  await test('mixed editor rejects oversized input without truncation or implicit Enter submission',async()=>{
    await action('mixed','field.prev');await action('mixed','form.accept');const input=article('mixed').getByRole('textbox',{name:'文本草稿',exact:true});
    await input.fill('12345678901');assert.equal((await state('mixed')).vars.titleDraft,'用户😀');assert((await article('mixed').locator('[aria-label="文本草稿输入"] [role="status"]').textContent()).includes('未修改草稿'));
    await input.fill('新的');await input.press('Enter');assert.equal((await state('mixed')).vars.title,'用户😀');assert.equal((await state('mixed')).vars.titleDraft,'新的');
    await action('mixed','form.cancel');assert.equal((await state('mixed')).vars.titleDraft,'用户😀');
  });
  await test('IME composition is draft-only and stale completion cannot edit a different field',async()=>{
    await action('mixed','form.accept');const input=article('mixed').getByRole('textbox',{name:'文本草稿',exact:true});
    await input.evaluate(element=>{
      element.dispatchEvent(new CompositionEvent('compositionstart',{bubbles:true}));
      const setter=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set;setter.call(element,'输入中');
      element.dispatchEvent(new InputEvent('input',{bubbles:true,inputType:'insertCompositionText',data:'输入中',isComposing:true}));
    });
    assert.equal((await state('mixed')).vars.titleDraft,'用户😀');
    await input.evaluate(element=>element.dispatchEvent(new CompositionEvent('compositionend',{bubbles:true,data:'输入中'})));
    await page.waitForTimeout(20);assert.equal((await state('mixed')).vars.titleDraft,'输入中');assert.equal((await state('mixed')).vars.title,'用户😀');
    await page.evaluate(()=>{const element=document.querySelector('[data-fixture="mixed"] input');window.__staleFormInput=element;element.dispatchEvent(new CompositionEvent('compositionstart',{bubbles:true}));});
    await page.evaluate(()=>window.__frameworkUi.drivers.mixed.action('field.next'));await page.waitForTimeout(20);
    await page.evaluate(()=>{window.__staleFormInput.value='过期输入';window.__staleFormInput.dispatchEvent(new CompositionEvent('compositionend',{bubbles:true,data:'过期输入'}));delete window.__staleFormInput;});
    assert.equal((await state('mixed')).vars.titleDraft,'输入中');assert.equal((await state('mixed')).vars.amountDraft,4);
    await action('mixed','form.cancel');
    await article('mixed').screenshot({path:join(evidence,`${label}-mixed-form.png`)});
    await action('mixed','field.prev');await action('mixed','form.accept');assert.equal(await article('mixed').locator('[aria-label="文本草稿输入"] [role="status"]').count(),0);
    await article('mixed').getByRole('textbox',{name:'文本草稿',exact:true}).fill('😀');
    await article('mixed').screenshot({path:join(evidence,`${label}-mixed-editing.png`)});await action('mixed','form.cancel');
  });
  await article('form').screenshot({path: join(evidence, `${label}-form.png`)});
  await article('preview').screenshot({path: join(evidence, `${label}-preview.png`)});
  assert.deepEqual(errors, []); assert.deepEqual(blocked, []);
  writeFileSync(join(evidence, `${label}.json`), JSON.stringify({passed: results.length, results, pageErrors: errors, blockedExternalRequests: blocked, browser: browser.version(), fixture: url.href, tauri: false, userData: false, hardware: false}, null, 2));
} catch (error) {
  await page.screenshot({path: join(evidence, `${label}-failure.png`), fullPage: true}).catch(() => {});
  writeFileSync(join(evidence, `${label}.json`), JSON.stringify({passed: results.length, results, error: String(error.stack), pageErrors: errors, blockedExternalRequests: blocked}, null, 2));
  throw error;
} finally {await browser.close();}
