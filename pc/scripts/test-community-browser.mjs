// Isolated UI contract test. Synthetic works never touch the public service.
import { chromium } from "playwright";
import assert from "node:assert/strict";
const base = "http://127.0.0.1:4177";
const browser = await chromium.launch({ headless:true, executablePath:process.env.PET_TEST_CHROMIUM || "/Applications/360Chrome.app/Contents/MacOS/360Chrome" });
try {
  const page=await browser.newPage({viewport:{width:1360,height:1000}});
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.route('**/__community-test',route=>route.fulfill({contentType:'text/html',body:'<html lang="zh"><body style="margin:32px"><div id="root"></div></body></html>'}));
  await page.goto(base+'/__community-test');
  await page.evaluate(async()=>{
    const Refresh=(await import('/@react-refresh')).default;Refresh.injectIntoGlobalHook(window);window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>t=>t;window.__vite_plugin_react_preamble_installed__=true;
    window.communityCalls=[];window.communityLiked=false;window.communityMine=[];window.communityComments=[];
    const work={id:'test-focus',name:'专注一刻',version:'1.1.0',author:'测试作者',summary:'给桌面一个安静的专注角落',category:'productivity',description:'短按 SW1 开始，退出遵循全局设置。',releaseNotes:'优化布局与计时。',license:'MIT',covers:[],requirements:{runtime:'p4-bounded-runtime-v4',pcRequired:false},updatedAt:1780000000,available:true,downloads:12,likes:4,commentCount:0,packageBytes:6000};
    window.__TAURI_INTERNALS__={invoke:async(command,args)=>{
      window.communityCalls.push([command,args]);if(command==='stock_watchlist_status')return {quotes:[]};
      if(command!=='community_request')throw Error('Unexpected command '+command);
      const input=args.input;await new Promise(r=>setTimeout(r,10));
      switch(input.action){
        case 'local':return {owned:{},downloaded:{}};
        case 'browse':return {items:input.query==='不存在'?[]:[work],nextOffset:null,total:1};
        case 'rankings':return {items:[{...work,rankValue:12}]};
        case 'detail':return {...work,liked:window.communityLiked};
        case 'comments':return {items:window.communityComments,nextOffset:null};
        case 'mine':return {items:window.communityMine};
        case 'like':window.communityLiked=input.active;return {active:input.active,likes:5};
        case 'comment':window.communityComments.push({id:crypto.randomUUID(),name:input.name,text:input.text,at:1780000000});return {ok:true};
        case 'prepare':return {id:'test-focus',name:work.name,version:'1.1.0',description:work.summary,canPublish:true,hasLicense:true,contentHash:'b'.repeat(64),files:['component.json','LICENSE.txt'],packageBytes:6000,warning:'扫描不能替代人工检查。'};
        case 'publish':window.communityMine=[{...work,status:'pending',available:false}];return {...work,status:'pending'};
        case 'download':return {entry:{id:'test-focus',name:work.name,version:'1.1.0',isLocal:true,libraryPath:'/test/library'},work};
        default:throw Error('Unexpected action '+input.action);
      }
    }};
    const React=(await import('/node_modules/.vite/deps/react.js')).default;
    const dom=await import('/node_modules/.vite/deps/react-dom_client.js');
    const {default:Community}=await import('/src/component-center/CommunityCenter.jsx');
    const {BUILTIN_COMPONENT_CENTER}=await import('/src/fixtures.js');
    const css=document.createElement('link');css.rel='stylesheet';css.href='/src/styles.css';document.head.append(css);
    const items=BUILTIN_COMPONENT_CENTER.components.filter(x=>['stock-watchlist','wooden-fish'].includes(x.id));
    items.push({id:'test-creation',name:'我的创作',version:'1.0.0',isLocal:true,libraryPath:'/test/creation'});
    function Test(){const [local,setLocal]=React.useState(items);return React.createElement(Community,{localItems:local,inventory:{freshness:'live',items:[]},onOpenLocal:()=>{window.openedLocal=true;},onCreate:()=>{window.created=true;},onImported:entry=>setLocal(old=>[...old,entry])},React.createElement('div',null,'原有本地组件与按键配置保留'));}
    (dom.createRoot||dom.default.createRoot)(document.getElementById('root')).render(React.createElement(Test));
  });
  await page.getByRole('heading',{name:'社区新鲜玩法'}).waitFor();
  await page.getByRole('heading',{name:'专注一刻',exact:true}).waitFor();
  await page.getByRole('button',{name:'收藏 专注一刻',exact:true}).click();
  await page.getByRole('tab',{name:'我的收藏'}).click();
  await page.getByRole('heading',{name:'专注一刻',exact:true}).click();
  await page.getByRole('dialog').waitFor();
  await page.getByRole('button',{name:'下载到我的组件'}).click();
  await page.getByRole('button',{name:/配置与同步到设备/}).waitFor();
  assert.equal(await page.evaluate(()=>window.communityCalls.some(([name])=>name==='install_clawpkg_over_usb')),false);
  await page.getByLabel('公开显示名',{exact:true}).fill('UI 测试');
  await page.getByLabel('分享体验',{exact:true}).fill('<img src=x onerror=alert(1)>');
  await page.getByLabel(/我同意公开以上显示名/).check();
  await page.getByRole('button',{name:'发表评论',exact:true}).click();
  await page.getByText('<img src=x onerror=alert(1)>',{exact:true}).waitFor();
  assert.equal(await page.locator('.community-comments img').count(),0);
  await page.getByRole('button',{name:'关闭',exact:true}).click();
  await page.getByRole('tab',{name:'我的组件'}).click();
  await page.getByText('原有本地组件与按键配置保留').waitFor();
  await page.getByRole('button',{name:'发布作品'}).click();
  await page.locator('.community-publish select').first().selectOption('/test/creation');
  await page.getByText('component.json',{exact:true}).waitFor({state:'attached'});
  await page.getByLabel('公开作者名',{exact:true}).fill('作者');
  await page.getByLabel(/许可证名称/).fill('MIT');
  await page.getByLabel('本次更新说明',{exact:true}).fill('首次发布');
  const png=Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl2nL8AAAAASUVORK5CYII=','base64');
  await page.locator('input[type=file]').setInputFiles({name:'fixture.png',mimeType:'image/png',buffer:png});
  assert.equal(await page.getByRole('button',{name:/提交审核/}).isDisabled(),true);
  for(const checkbox of await page.locator('.community-publish-checks input').all()) await checkbox.check();
  await page.getByRole('button',{name:/提交审核/}).click();
  await page.getByText('待审核',{exact:true}).waitFor();
  assert.equal(await page.getByRole('button',{name:'等待审核'}).isDisabled(),true);
  for(const width of [1440,1024,736,360]){
    await page.setViewportSize({width,height:900});await page.getByRole('tab',{name:'发现玩法'}).click();
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false,`overflow ${width}`);
  }
  await page.setViewportSize({width:1360,height:1000});
  await page.screenshot({path:'/tmp/hachimo-community-implemented.png',fullPage:true});
  assert.deepEqual(errors,[]);
  console.log('PASS: discovery/favorites/details, verified-download separation, plaintext comments, publish consent/moderation, local tab and responsive widths');
}finally{await browser.close();}
