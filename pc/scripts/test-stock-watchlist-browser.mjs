// Synthetic UI regression. No real network requests, settings writes or device access.
import { chromium } from "playwright";
import assert from "node:assert/strict";
const base = process.env.PET_TEST_URL || "http://127.0.0.1:4175";
assert.equal(new URL(base).hostname, "127.0.0.1");
const browser = await chromium.launch({ headless: true, executablePath: process.env.PET_TEST_CHROMIUM || undefined });
try {
  const page = await browser.newPage({ viewport: { width: 1120, height: 780 } });
  const errors = []; page.on("pageerror", e => errors.push(e.message));
  await page.route("**/__test-stocks", r => r.fulfill({ contentType: "text/html", body: '<html lang="zh"><body style="margin:24px;background:#f7f8fa"><div id="root"></div></body></html>' }));
  await page.goto(`${base}/__test-stocks`);
  await page.evaluate(async () => {
    const Refresh = (await import("/@react-refresh")).default;
    Refresh.injectIntoGlobalHook(window); window.$RefreshReg$ = () => {}; window.$RefreshSig$ = () => type => type; window.__vite_plugin_react_preamble_installed__ = true;
    window.stockTest = { settings: { symbols: [] }, quotes: [], error: "", fetchedAtMs: 0 };
    window.__TAURI_INTERNALS__ = { invoke: async (command, args) => {
      if (command === "stock_watchlist_status") return structuredClone(window.stockTest);
      if (command === "stock_search") {
        window.stockSearchQueries = [...(window.stockSearchQueries || []), args.query];
        if (args.query === "腾讯") return [{ symbol: "hk00700", name: "腾讯控股", market: "港股" }];
        if (args.query === "茅台") return [{ symbol: "sh600519", name: "贵州茅台", market: "沪市" }];
        if (args.query === "旧请求") return await new Promise(resolve => { window.resolveOldSearch = () => resolve([{symbol:"sh600001",name:"过时结果",market:"沪市"}]); });
        return [];
      }
      if (command === "stock_quote_lookup") {
        if (args.symbol === "sh000000") throw "未找到有效行情，请检查市场和股票代码";
        return { symbol: args.symbol, name: args.symbol === "sh600000" ? "测试沪市股票" : "测试港股" };
      }
      if (command === "stock_watchlist_save") {
        window.stockTest.settings = args.input;
        window.stockTest.quotes = args.input.symbols.map(symbol => ({ symbol, name: symbol === "sh600000" ? "测试沪市股票" : "测试港股", price: "12.30", changePercent: "1.20", quoteTime: "2026-09-21 12:00:00", currency: symbol.startsWith("hk") ? "HKD" : "CNY", stale: false, tone: 1 }));
        return structuredClone(window.stockTest);
      }
      if (command === "stock_watchlist_refresh") { window.stockTest.quotes.forEach(q => q.stale = true); window.stockTest.error = "行情连接失败，请检查网络后重试"; return structuredClone(window.stockTest); }
      throw new Error(`Unexpected command: ${command}`);
    } };
    const React = (await import("/node_modules/.vite/deps/react.js")).default;
    const dom = await import("/node_modules/.vite/deps/react-dom_client.js");
    const { default: ComponentPreviewModal } = await import("/src/component-center/ComponentPreviewModal.jsx");
    const css = document.createElement("link"); css.rel = "stylesheet"; css.href = "/src/styles.css"; document.head.append(css);
    const root = (dom.createRoot || dom.default.createRoot)(document.getElementById("root"));
    window.renderStockModal = (id = "stock-watchlist") => root.render(React.createElement(ComponentPreviewModal, {
      component: { id, name: id === "stock-watchlist" ? "自选股行情" : "其他组件", dashboard: {} },
      kind: "tool", isLocal: true, deviceConnected: true,
      usb: { connected: true, capabilities: {} }, onClose: () => root.render(null),
    }));
    window.renderStockModal();
  });
  await page.getByRole("dialog").waitFor();
  assert.equal(await page.getByRole("dialog").locator(".stock-watchlist").count(), 1);
  assert.equal(await page.getByRole("button", { name: "添加行情组件", exact: true }).count(), 0);
  await page.getByText("还没有自选股。", { exact: false }).waitFor();
  await page.getByText("当前设备固件尚不支持", { exact: false }).waitFor();
  await page.getByLabel("股票名称或代码", { exact: true }).fill("600000");
  await page.getByRole("button", { name: "添加股票", exact: true }).click();
  await page.getByText("测试沪市股票", { exact: true }).waitFor();
  const colors = await page.getByRole("row").filter({ hasText: "测试沪市股票" }).locator("td").evaluateAll(cells => cells.map(c => getComputedStyle(c).color));
  assert.equal(colors[1], "rgb(228, 78, 80)"); assert.equal(colors[2], colors[1]); assert.notEqual(colors[0], colors[1]);
  await page.getByLabel("股票市场").selectOption("hk");
  await page.getByLabel("股票名称或代码", { exact: true }).fill("腾讯");
  await page.getByRole("button", { name: /腾讯控股.*港股.*hk00700.*添加/ }).click();
  await page.getByText("测试港股", { exact: true }).waitFor();
  await page.getByLabel("股票名称或代码", { exact: true }).fill("00700");
  // Already-added code is still rejected by the normal manual add path.
  await page.getByRole("button", { name: "添加股票", exact: true }).click();
  await page.getByText("测试港股", { exact: true }).waitFor();
  await page.getByLabel("上移 hk00700", { exact: true }).click();
  assert.deepEqual(await page.evaluate(() => window.stockTest.settings.symbols), ["hk00700", "sh600000"]);
  await page.getByLabel("股票名称或代码", { exact: true }).fill("00700");
  await page.getByRole("button", { name: "添加股票", exact: true }).click();
  await page.getByRole("alert").filter({ hasText: "已经在自选股中" }).waitFor();
  await page.getByLabel("删除 sh600000", { exact: true }).click();
  await page.getByText("测试沪市股票", { exact: true }).waitFor({ state: "detached" });
  await page.getByRole("button", { name: "刷新行情", exact: true }).click();
  await page.getByText("已过期", { exact: false }).waitFor();
  await page.getByLabel("股票名称或代码", { exact: true }).fill("旧请求");
  await page.waitForFunction(() => typeof window.resolveOldSearch === "function");
  await page.getByLabel("股票名称或代码", { exact: true }).fill("茅台");
  await page.getByRole("button", { name: /贵州茅台.*沪市.*sh600519/ }).waitFor();
  await page.evaluate(() => window.resolveOldSearch());
  assert.equal(await page.getByText("过时结果").count(), 0);
  if (process.env.PET_TEST_SCREENSHOT) await page.screenshot({ path: process.env.PET_TEST_SCREENSHOT });
  await page.setViewportSize({ width: 740, height: 780 });
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
  await page.getByRole("button", { name: "关闭组件详情", exact: true }).click();
  await page.getByRole("dialog").waitFor({ state: "detached" });
  await page.evaluate(() => window.renderStockModal("other-tool"));
  await page.getByRole("dialog").waitFor();
  assert.equal(await page.locator(".stock-watchlist").count(), 0);
  assert.deepEqual(errors, []);
  console.log("PASS: stock details only, Chinese typeahead, stale-result isolation, add/reorder/delete, stale quote, firmware gate and narrow modal");
} finally { await browser.close(); }
