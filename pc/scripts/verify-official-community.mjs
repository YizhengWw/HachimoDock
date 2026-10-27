// Read-only deployment audit: no download tickets, counters, or test postings.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {chromium} from 'playwright';
const origin='https://115.191.67.48',directory=path.resolve(process.argv[2]);
const expected=JSON.parse(fs.readFileSync(path.join(directory,'canonical/catalog.json')));
const before=JSON.parse(fs.readFileSync(path.join(directory,'before.json')));
const sha=b=>createHash('sha256').update(b).digest('hex');
async function get(route){const response=await fetch(origin+route,{signal:AbortSignal.timeout(30000)});assert.equal(response.status,200,route);return response;}
const catalog=await (await get('/api/v1/works?limit=50')).json();
assert.equal(catalog.items.length,expected.length);
assert.equal(catalog.items.some(w=>w.id==='music-player'),false);
for(const entry of expected){
 const work=catalog.items.find(w=>w.id===entry.id);assert(work);
 for(const key of ['sha256','contentHash','version','official','packageBytes','covers','files','requirements','author','category','name'])assert.deepEqual(work[key],entry[key],entry.id+' '+key);
 const prior=before.items.find(w=>w.id===entry.id);assert(prior);
 for(const key of ['downloads','likes','commentCount','createdAt'])assert.deepEqual(work[key],prior[key],entry.id+' preserved '+key);
 assert.notEqual(work.sha256,prior.sha256,entry.id+' package updated');
 // Identical current art can retain its content hash; versions/packages still advance.
 const bytes=Buffer.from(await (await get('/api/v1/packages/'+work.sha256)).arrayBuffer());
 assert.equal(sha(bytes),work.sha256);assert.equal(bytes.length,work.packageBytes);
 for(const image of work.covers)assert.equal(sha(Buffer.from(await (await get('/api/v1/images/'+image)).arrayBuffer())),image);
 console.log('Verified package and cover:',entry.id);
}
for(const id of ['music-player','hachimi-notes','hachimi-agent','hachimi-desk'])assert.equal((await fetch(origin+'/api/v1/works/'+id)).status,404);
const browser=await chromium.launch({headless:true,executablePath:process.env.PET_TEST_CHROMIUM||'/Applications/360Chrome.app/Contents/MacOS/360Chrome'});
try{
 const page=await browser.newPage({viewport:{width:1360,height:1050}}),errors=[];
 page.on('pageerror',e=>errors.push(e.message));
 await page.exposeFunction('communityReadOnly',async input=>{
  if(input.action==='local')return {owned:{},downloaded:{}};
  if(input.action==='image'){
   const res=await get('/api/v1/images/'+input.id);return {dataUrl:'data:image/jpeg;base64,'+Buffer.from(await res.arrayBuffer()).toString('base64')};
  }
  const params=new URLSearchParams();for(const key of ['query','category','sort','offset','limit','window','metric'])if(input[key]!=null)params.set(({query:'q',sort:'order'})[key]||key,input[key]);
  const route={browse:'/api/v1/works',rankings:'/api/v1/rankings',detail:'/api/v1/works/'+input.id,comments:'/api/v1/works/'+input.id+'/comments'}[input.action];
  if(!route)throw Error('Read-only action only: '+input.action);
  return (await get(route+'?'+params)).json();
 });
 await page.route('**/__official-live',route=>route.fulfill({contentType:'text/html',body:'<html lang="zh"><body style="margin:24px"><div id="root"></div></body></html>'}));
 await page.goto('http://127.0.0.1:4197/__official-live');
 await page.evaluate(async()=>{
  const Refresh=(await import('/@react-refresh')).default;Refresh.injectIntoGlobalHook(window);window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>t=>t;window.__vite_plugin_react_preamble_installed__=true;
  window.__TAURI_INTERNALS__={invoke:async(command,args)=>{if(command==='community_request')return window.communityReadOnly(args.input);if(command==='stock_watchlist_status')return {quotes:[]};throw Error('Unexpected command');}};
  const React=(await import('/node_modules/.vite/deps/react.js')).default,dom=await import('/node_modules/.vite/deps/react-dom_client.js');
  const {default:Center}=await import('/src/component-center/CommunityCenter.jsx'),{BUILTIN_COMPONENT_CENTER}=await import('/src/fixtures.js');
  const css=document.createElement('link');css.rel='stylesheet';css.href='/src/styles.css';document.head.append(css);
  (dom.createRoot||dom.default.createRoot)(document.getElementById('root')).render(React.createElement(Center,{localItems:BUILTIN_COMPONENT_CENTER.components.filter(c=>c.id!=='music-player'),inventory:{freshness:'unsupported',items:[]},onOpenLocal:()=>{},onCreate:()=>{},onImported:()=>{}}));
 });
 await page.getByRole('heading',{name:'社区新鲜玩法'}).waitFor();
 await page.locator('.community-card').first().waitFor();
 console.log('Live community page loaded');
 await page.waitForTimeout(800);
 let seen=0,pageNumber=1;
 while(true){
  const cards=await page.locator('.community-card').all();
  for(const card of cards){
   console.log('Checking visible card:',(await card.innerText()).slice(0,40));
   await card.scrollIntoViewIfNeeded();await card.locator('img').waitFor();
   await card.locator('img').evaluate(im=>im.decode());
   assert.equal(await card.locator('img').evaluate(im=>im.complete&&im.naturalWidth===640&&im.naturalHeight===480),true);
  }
  seen+=cards.length;
  await page.evaluate(()=>scrollTo(0,0));
  assert.equal(await page.getByText('随身听',{exact:true}).count(),0);
  await page.screenshot({path:path.join(directory,`community-live-${pageNumber}.png`),fullPage:true});
  const next=page.getByRole('button',{name:'下一页',exact:true});
  if(!await next.count())break;
  const firstText=await page.locator('.community-card').first().innerText();
  await next.click();
  await page.waitForFunction(t=>document.querySelector('.community-card')?.innerText!==t,firstText);
  await page.locator('.community-card').first().waitFor();
  pageNumber++;
  if(pageNumber>5)throw Error('Pagination loop');
 }
 assert.equal(seen,expected.length);
 assert.deepEqual(errors,[]);
 const after=await (await get('/api/v1/works?limit=30')).json();
 for(const prior of before.items)for(const key of ['downloads','likes','commentCount'])assert.equal(after.items.find(w=>w.id===prior.id)[key],prior[key]);
 fs.writeFileSync(path.join(directory,'verification.json'),JSON.stringify({publicWorks:expected.length,restrictedPending:['music-player','hachimi-notes'],unlisted:['hachimi-agent','hachimi-desk'],allPackageHashesVerified:true,allCoverHashesVerified:true,allMetadataVerified:true,liveUiCards:seen,liveUiPages:pageNumber,downloadCountersUnchanged:true,tlsVerification:true,verifiedAt:new Date().toISOString()},null,2)+'\n');
 console.log(`PASS: ${seen} public packages/images match audited content; protected works remain private; real-service UI verified`);
}finally{await browser.close();}
