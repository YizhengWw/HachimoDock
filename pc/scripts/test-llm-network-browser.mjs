// Run against a local Vite server: PET_TEST_URL=http://127.0.0.1:4179 node scripts/test-llm-network-browser.mjs
// Optional PET_TEST_BROWSER selects an already installed headless Chromium executable.
import { chromium } from 'playwright';
import assert from 'node:assert/strict';
const base = process.env.PET_TEST_URL || 'http://127.0.0.1:4173';
assert.equal(new URL(base).hostname, '127.0.0.1', 'Only a local development server is allowed');
const browser = await chromium.launch({ headless: true, ...(process.env.PET_TEST_BROWSER ? { executablePath: process.env.PET_TEST_BROWSER } : {}) });
try {
  const page = await browser.newPage({ viewport: { width: 1000, height: 900 } });
  await page.route('**/__test-network', r => r.fulfill({ contentType: 'text/html', body: '<html><body style="padding:24px"><div id="root"></div></body></html>' }));
  await page.goto(`${base}/__test-network`);
  await page.evaluate(async () => {
    const Refresh = (await import('/@react-refresh')).default;
    Refresh.injectIntoGlobalHook(window);
    window.$RefreshReg$ = () => {};
    window.$RefreshSig$ = () => type => type;
    window.__vite_plugin_react_preamble_installed__ = true;
    window.calls = [];
    window.__TAURI_INTERNALS__ = { invoke: async (command, args) => {
      window.calls.push({ command, args });
      if (command === 'load_llm_network_settings') return { mode: 'system', proxyUrl: '', certificateConfigured: false };
      if (command === 'save_llm_network_settings') return { ...args.input, certificateConfigured: Boolean(args.input.caFile) };
      if (command === 'test_llm_network_settings') return [
        { provider: '火山方舟（豆包）', reachable: true, message: '已收到 HTTPS 响应' },
        { provider: 'DeepSeek', reachable: false, message: '访问被拒绝' },
      ];
      if (command === 'plugin:dialog|open') return '/test/company-public-ca.pem';
      throw new Error(command);
    } };
    const React = (await import('/node_modules/.vite/deps/react.js')).default;
    const dom = await import('/node_modules/.vite/deps/react-dom_client.js');
    const { createRoot } = dom.default || dom;
    const { default: Settings } = await import('/src/LlmNetworkSettings.jsx');
    const link = document.createElement('link'); link.rel = 'stylesheet'; link.href = '/src/styles.css'; document.head.append(link);
    createRoot(document.getElementById('root')).render(React.createElement(Settings));
  });
  await page.locator('summary').click();
  await page.getByLabel('连接方式').selectOption('manual');
  await page.getByLabel('HTTP / HTTPS 代理地址').fill('http://proxy.example:8080');
  assert.equal(await page.getByRole('button', { name: '测试已保存配置' }).isDisabled(), true);
  await page.getByRole('button', { name: '导入企业 CA 证书' }).click();
  await page.getByText('待导入：company-public-ca.pem').waitFor();
  await page.getByRole('button', { name: '保存网络设置' }).click();
  await page.getByText('已保存到这台电脑', { exact: false }).waitFor();
  await page.getByRole('button', { name: '测试已保存配置' }).click();
  await page.getByText('DeepSeek：访问被拒绝', { exact: false }).waitFor();
  assert.equal(await page.locator('.is-error').count(), 1);
  assert.equal(await page.locator('.is-success').count(), 1);
  if (process.env.PET_TEST_SCREENSHOT) await page.screenshot({ path: process.env.PET_TEST_SCREENSHOT, fullPage: true });
  await page.getByRole('button', { name: '移除导入的证书' }).click();
  await page.getByLabel('连接方式').selectOption('system');
  await page.getByRole('button', { name: '保存网络设置' }).click();
  await page.getByText('已保存到这台电脑', { exact: false }).waitFor();
  const calls = await page.evaluate(() => window.calls.filter(c => c.command === 'save_llm_network_settings'));
  assert.equal(calls[0].args.input.proxyUrl, 'http://proxy.example:8080');
  assert.equal(calls[0].args.input.caFile, '/test/company-public-ca.pem');
  assert.equal(calls.at(-1).args.input.clearCertificate, true);
  assert.equal(calls.at(-1).args.input.mode, 'system');
  console.log('PASS network UI: load, mode, proxy, CA import/removal, save-before-test, provider results');
} finally { await browser.close(); }
