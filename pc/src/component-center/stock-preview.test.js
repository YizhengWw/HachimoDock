import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { stockPreviewRows } from "./stock-preview.js";

test("default stock cover always shows Xiaomi and Alibaba without invented quotes", () => {
  const rows = stockPreviewRows(null);
  assert.deepEqual(rows.map(r => [r.symbol, r.name, r.price, r.change, r.tone]), [
    ["hk01810", "小米集团", "—", "—", 0], ["hk09988", "阿里巴巴", "—", "—", 0],
  ]);
  assert.ok(rows.every(r => !r.date && !r.available));
});
test("cover maps cached quotes by symbol, not order, and leaves other stocks alone", () => {
  const input = { settings: { symbols: ["usAAPL"] }, quotes: [
    {symbol:"hk09988",price:"100.00",changePercent:"-2.00",tone:-1,quoteTime:"2026-09-21 10:00:00"},
    {symbol:"hk01810",price:"20.00",changePercent:"1.20",tone:1,quoteTime:"2026-09-21 10:01:00"},
  ]};
  const before = structuredClone(input);
  const rows = stockPreviewRows(input);
  assert.deepEqual(rows.map(r => [r.price,r.change,r.tone]), [["20.00","+1.20%",1],["100.00","-2.00%",-1]]);
  assert.deepEqual(rows.map(r=>r.date),["2026-09-21","2026-09-21"]);
  assert.deepEqual(input,before);
});
test("stale/error snapshots are neutral and missing change is not displayed as zero", () => {
  const quotes = [{symbol:"hk01810",price:"20",changePercent:"",tone:1,stale:true}];
  assert.equal(stockPreviewRows({quotes})[0].tone,0);
  assert.equal(stockPreviewRows({quotes})[0].change,"—");
  assert.ok(stockPreviewRows({quotes,error:"offline"})[0].stale);
  assert.equal(stockPreviewRows({quotes:[{...quotes[0],changePercent:"0.00",tone:0,stale:false}]})[0].change,"0.00%");
});
test("preview only polls native cache while visible and does not mutate the watchlist", () => {
  const source = readFileSync(new URL("./StockScreenPreview.jsx",import.meta.url),"utf8");
  assert.deepEqual([...source.matchAll(/invoke\("([^"]+)"/g)].map(m=>m[1]),["stock_watchlist_status"]);
  assert.match(source,/!active \|\| !window\.__TAURI_INTERNALS__/);
  assert.match(source,/clearTimeout/);
  const shared = readFileSync(new URL("./DeviceScreenPreview.jsx",import.meta.url),"utf8");
  assert.match(shared,/component\.dataSource === "stocks.watchlist" \|\| component\.id === "stock-watchlist"/);
});
