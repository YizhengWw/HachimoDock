// Local UI regression only: synthetic catalog, no real keys or billable tasks.
import { chromium } from 'playwright';
import assert from 'node:assert/strict';
const base = process.env.PET_TEST_URL || 'http://127.0.0.1:4173';
assert.equal(new URL(base).hostname, '127.0.0.1');
const browser = await chromium.launch({ headless: true, ...(process.env.PET_TEST_BROWSER ? { executablePath: process.env.PET_TEST_BROWSER } : {}) });
try {
  const page = await browser.newPage({ viewport: { width: 850, height: 600 } });
  await page.route('**/__test-models', r => r.fulfill({ contentType: 'text/html', body: '<html><body style="padding:24px"><div id="root"></div></body></html>' }));
  await page.goto(`${base}/__test-models`);
  await page.evaluate(async () => {
    const Refresh = (await import('/@react-refresh')).default;
    Refresh.injectIntoGlobalHook(window);
    window.$RefreshReg$ = () => {};
    window.$RefreshSig$ = () => type => type;
    window.__vite_plugin_react_preamble_installed__ = true;
    window.failCatalog = false;
    window.__TAURI_INTERNALS__ = { invoke: async (command) => {
      if (command !== 'http_request_text') throw new Error('Unexpected command');
      if (window.failCatalog) throw new Error('HTTPS 证书校验失败：请检查云服务网络');
      return { status: 200, body: JSON.stringify({ data: [
        { id: 'doubao-seedance-1-0-pro-fast-251015' },
        { id: 'doubao-seedance-2-0-fast-260128' },
        { id: 'doubao-seedance-2-5-260628' },
      ] }) };
    } };
    const React = (await import('/node_modules/.vite/deps/react.js')).default;
    const dom = await import('/node_modules/.vite/deps/react-dom_client.js');
    const { VideoModelSelect } = await import('/src/VideoGenerationSettings.jsx');
    const link = document.createElement('link'); link.rel = 'stylesheet'; link.href = '/src/styles.css'; document.head.append(link);
    function Fixture() {
      const [model, setModel] = React.useState('');
      const onReady = React.useCallback(value => { window.modelReady = value; }, []);
      return React.createElement(VideoModelSelect, { apiKey: 'fixture-only', model, onModel: setModel, onReady });
    }
    (dom.default || dom).createRoot(document.getElementById('root')).render(React.createElement(Fixture));
  });
  const select = page.getByRole('combobox', { name: '视频生成模型' });
  await page.waitForFunction(() => window.modelReady === true);
  assert.equal(await select.inputValue(), 'doubao-seedance-2-0-fast-260128');
  assert.equal(await select.locator('option').count(), 4);
  await select.selectOption('doubao-seedance-2-5-260628');
  await page.getByRole('button', { name: '刷新可用模型' }).click();
  await page.waitForFunction(() => window.modelReady === true);
  assert.equal(await select.inputValue(), 'doubao-seedance-2-5-260628');
  await page.evaluate(() => { window.failCatalog = true; });
  await page.getByRole('button', { name: '刷新可用模型' }).click();
  await page.getByRole('alert').filter({ hasText: '证书校验失败' }).waitFor();
  await page.getByRole('checkbox').check();
  await page.getByRole('textbox', { name: '模型或接入点 ID' }).fill('ep-fixture');
  await page.waitForFunction(() => window.modelReady === true);
  console.log('PASS model UI: catalog, Fast default, manual selection, refresh preservation, actionable errors, endpoint fallback');
} finally { await browser.close(); }
