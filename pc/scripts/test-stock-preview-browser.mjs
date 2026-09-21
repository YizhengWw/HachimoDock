// Synthetic preview-only UI verification; no network quotes or user settings writes.
import { chromium } from "playwright";
import assert from "node:assert/strict";
const base = process.env.PET_TEST_URL || "http://127.0.0.1:4175";
assert.equal(new URL(base).hostname, "127.0.0.1");
const browser = await chromium.launch({ headless: true, executablePath: process.env.PET_TEST_CHROMIUM || undefined });
try {
  const page = await browser.newPage({ viewport: { width: 1000, height: 700 } });
  const errors=[]; page.on('pageerror', e=>errors.push(e.message));
  await page.route('**/__stock-cover', r=>r.fulfill({contentType:'text/html',body:'<html lang="zh"><body style="margin:24px;background:#f7f8fa"><div id="root"></div></body></html>'}));
  await page.goto(`${base}/__stock-cover`);
  await page.evaluate(async()=>{
    const Refresh=(await import('/@react-refresh')).default;
    Refresh.injectIntoGlobalHook(window);window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>t=>t;window.__vite_plugin_react_preamble_installed__=true;
    window.previewStatus={quotes:[]};window.previewCalls=[];
    window.__TAURI_INTERNALS__={invoke:async command=>{
      window.previewCalls.push(command);
      if(command!=='stock_watchlist_status')throw Error('Unexpected mutation');
      return structuredClone(window.previewStatus);
    }};
    const React=(await import('/node_modules/.vite/deps/react.js')).default;
    const dom=await import('/node_modules/.vite/deps/react-dom_client.js');
    const {default:CandidateCard}=await import('/src/component-center/CandidateCard.jsx');
    const css=document.createElement('link');css.rel='stylesheet';css.href='/src/styles.css';document.head.append(css);
    const root=(dom.createRoot||dom.default.createRoot)(document.getElementById('root'));
    root.render(React.createElement('div',{style:{width:'min(430px, 100%)'}},React.createElement(CandidateCard,{component:{id:'stock-watchlist',name:'自选股行情',dashboard:{visualStyle:'clean',visualLayout:'tool'},goal:'预览默认股票，在详情中管理实际自选股。'},kind:'tool',onClick:()=>{}})));
  });
  await page.getByText('小米集团',{exact:true}).waitFor();
  await page.getByText('阿里巴巴',{exact:true}).waitFor();
  assert.equal(await page.locator('.cds-stocks__row').count(),2);
  assert.equal(await page.locator('.cds-stocks__row [data-tone]').allTextContents().then(a=>a.join(',')),'—,—,—,—');
  await page.screenshot({path:'/tmp/pet-stock-cover-empty.png'});
  await page.evaluate(()=>window.previewStatus={quotes:[
    {symbol:'hk01810',price:'20.00',changePercent:'1.20',tone:1,quoteTime:'2026-09-21 10:00:00'},
    {symbol:'hk09988',price:'100.00',changePercent:'-2.00',tone:-1,quoteTime:'2026-09-21 10:00:00'},
  ]});
  await page.getByText('+1.20%',{exact:true}).waitFor();
  const colors=await page.locator('.cds-stocks__row').first().locator('[role=cell]').evaluateAll(c=>c.map(e=>getComputedStyle(e).color));
  assert.equal(colors[1],'rgb(255, 115, 117)');assert.equal(colors[2],colors[1]);assert.notEqual(colors[0],colors[1]);
  assert.equal(await page.getByText('2026-09-21',{exact:true}).count(),1);
  await page.screenshot({path:'/tmp/pet-stock-cover-quotes.png'});
  await page.setViewportSize({width:360,height:650});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
  await page.screenshot({path:'/tmp/pet-stock-cover-narrow.png'});
  const dimensions=await page.locator('.cds-stocks').evaluate(el=>({height:el.clientHeight,scrollHeight:el.scrollHeight,width:el.clientWidth,scrollWidth:el.scrollWidth}));
  const fits=dimensions.scrollHeight<=dimensions.height+1 && dimensions.scrollWidth<=dimensions.width+1;
  assert.ok(fits,`Preview should fit its LCD area: ${JSON.stringify(dimensions)}`);
  await page.evaluate(()=>window.previewStatus={...window.previewStatus,error:'offline'});
  await page.getByText('缓存行情 · 已过期',{exact:true}).waitFor();
  assert.equal(await page.locator('.cds-stocks [data-tone="0"]').count(),4);
  assert.deepEqual([...new Set(await page.evaluate(()=>window.previewCalls))],['stock_watchlist_status']);
  assert.deepEqual(errors,[]);
  console.log('PASS: default rows, real-cache mapping, placeholders, price-only color, single date, narrow layout, stale state, read-only access');
} finally { await browser.close(); }
