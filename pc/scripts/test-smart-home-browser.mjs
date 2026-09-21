// Synthetic UI regression. Never reads real tokens or sends physical device commands.
import { chromium } from "playwright";
import assert from "node:assert/strict";
const base = process.env.PET_TEST_URL || "http://127.0.0.1:4173";
assert.equal(new URL(base).hostname, "127.0.0.1");
const browser = await chromium.launch({ headless: true, ...(process.env.PET_TEST_BROWSER ? { executablePath: process.env.PET_TEST_BROWSER } : {}) });
try {
  const page = await browser.newPage({ viewport: { width: 1180, height: 820 } });
  const errors = []; page.on("pageerror", e => errors.push(e.message));
  await page.route("**/__test-home", route => route.fulfill({ contentType: "text/html", body: '<html lang="zh"><body style="background:#f7f8fa;padding:24px"><div id="root"></div></body></html>' }));
  await page.goto(`${base}/__test-home`);
  await page.evaluate(async () => {
    const Refresh = (await import("/@react-refresh")).default;
    Refresh.injectIntoGlobalHook(window); window.$RefreshReg$ = () => {}; window.$RefreshSig$ = () => type => type; window.__vite_plugin_react_preamble_installed__ = true;
    const data = { connected: false, devices: [], scenes: [], pending: [], tasks: [], records: [] };
    window.homeCalls = [];
    window.__TAURI_INTERNALS__ = { invoke: async (command, args = {}) => {
      window.homeCalls.push({ command, args });
      if (command === "open_external_url") return;
      if (command === "smart_home_cancel") return;
      if (command === "smart_home_chat") return "请先说明要调整哪一个设备。";
      if (command !== "smart_home_request") throw new Error("Unexpected command");
      const { operation, input } = args;
      if (operation === "status") return structuredClone(data);
      if (operation === "login") return { url: "https://account.xiaomi.com/oauth2/authorize?fixture=true" };
      if (operation === "authorize") { data.connected = true; return data; }
      if (operation === "sync") { data.devices = [{ did: "speaker-1", name: "小爱音箱", homeId: "h1", home: "我的家", room: "客厅", model: "fixture.speaker", online: true }]; data.scenes = [{ id: "scene-1", name: "观影模式", home: "我的家" }]; data.syncedAt = Date.now() / 1000; return { warnings: [] }; }
      if (operation === "describe") return { properties: [{ siid: 2, piid: 1, name: "音量", service: "音箱", format: "uint8", range: [0, 100, 1], access: ["read", "write"] }], actions: [{ siid: 3, aiid: 1, name: "暂停", service: "音箱", in: [] }] };
      if (operation === "read") return [{ siid: 2, piid: 1, value: 20, code: 0 }];
      if (operation === "plan") { data.pending.push({ ...input, id: "plan-1" }); return { status: "awaiting_confirmation" }; }
      if (operation === "approve") { const p = data.pending.shift(); data.records = [{ id: p.id, title: p.title, at: Date.now() / 1000, status: "completed", results: p.steps.map(step => ({ step, result: { status: "verified" } })) }]; return data.records[0]; }
      if (operation === "save_task") { data.tasks.push({ ...data.pending[0], id: "saved-1" }); return data; }
      if (operation === "reject") { data.pending = []; return data; }
      if (operation === "delete_task") { data.tasks = []; return data; }
      throw new Error(`Unexpected operation ${operation}`);
    } };
    const React = (await import("/node_modules/.vite/deps/react.js")).default;
    const dom = await import("/node_modules/.vite/deps/react-dom_client.js");
    const { default: SmartHome } = await import("/src/SmartHome.jsx");
    const css = document.createElement("link"); css.rel = "stylesheet"; css.href = "/src/styles.css"; document.head.append(css);
    (dom.default || dom).createRoot(document.getElementById("root")).render(React.createElement(SmartHome));
  });
  await page.getByText("智能家居能力来自 Xiaomi MiLoCo", { exact: false }).waitFor();
  assert.equal(await page.getByText(/无需逐个勾选|登录凭据仅保存在本机|升级后若连接失效/).count(), 0);
  if (process.env.PET_TEST_WELCOME_SCREENSHOT) await page.screenshot({ path: process.env.PET_TEST_WELCOME_SCREENSHOT, fullPage: true });
  await page.getByRole("button", { name: "连接米家账号", exact: true }).click();
  await page.getByRole("textbox", { name: "授权结果" }).fill("fixture-only");
  await page.getByRole("button", { name: "完成连接", exact: true }).click();
  await page.getByRole("heading", { name: "小爱音箱" }).waitFor();
  assert.equal(await page.getByRole("checkbox").count(), 0);
  await page.getByRole("button", { name: "查看状态与控制" }).click();
  await page.getByText("当前：20").waitFor();
  await page.getByRole("spinbutton", { name: "音量" }).fill("25");
  await page.getByRole("button", { name: "加入任务", exact: true }).first().click();
  await page.getByRole("button", { name: "查看任务草稿" }).click();
  await page.getByRole("button", { name: "添加等待 2 秒" }).click();
  await page.getByRole("button", { name: "检查任务", exact: true }).click();
  await page.getByRole("button", { name: "保存为常用任务" }).click();
  await page.getByRole("button", { name: "确认执行", exact: true }).click();
  await page.getByRole("heading", { name: "执行记录", exact: true }).waitFor();
  assert.equal(await page.getByText("已核验", { exact: true }).count(), 2);
  await page.getByRole("tab", { name: "场景与任务" }).click();
  await page.getByRole("textbox", { name: "家居需求" }).fill("帮我调整音箱");
  await page.getByRole("button", { name: "交给哈基米" }).click();
  await page.getByText("请先说明要调整哪一个设备。", { exact: false }).waitFor();
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
  if (process.env.PET_TEST_SCREENSHOT) await page.screenshot({ path: process.env.PET_TEST_SCREENSHOT, fullPage: true });
  await page.setViewportSize({ width: 820, height: 700 });
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
  assert.deepEqual(errors, []);
  assert.equal(await page.evaluate(() => window.homeCalls.filter(c => c.command === "smart_home_request" && c.args.operation === "approve").length), 1);
  console.log("PASS smart-home UI: authorization, all devices/scenes available without checkboxes, capability inputs, draft, approval, saved task, receipts, chat, responsive layout; no physical calls");
} finally { await browser.close(); }
