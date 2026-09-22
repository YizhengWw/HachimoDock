// Synthetic settings/UI regression; no credentials, network probes, or device operations.
import { chromium } from 'playwright';
import assert from 'node:assert/strict';
const base = process.env.PET_TEST_URL || 'http://127.0.0.1:4173';
assert.equal(new URL(base).hostname, '127.0.0.1');
const browser = await chromium.launch({ headless: true, ...(process.env.PET_TEST_BROWSER ? { executablePath: process.env.PET_TEST_BROWSER } : {}) });
try {
  const page = await browser.newPage({ viewport: { width: 1180, height: 900 } });
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.route('**/__test-settings', r => r.fulfill({ contentType: 'text/html', body: '<html lang="zh"><body style="padding:24px;background:#f7f8fa"><div id="root"></div></body></html>' }));
  await page.goto(`${base}/__test-settings`);
  await page.evaluate(async () => {
    const Refresh = (await import('/@react-refresh')).default;
    Refresh.injectIntoGlobalHook(window); window.$RefreshReg$ = () => {}; window.$RefreshSig$ = () => type => type;
    window.__vite_plugin_react_preamble_installed__ = true;
    window.modelTestSettings = { llmConfigured: true, llmApiKeyMasked: 'tes…mask',
      llmApiKeyMasks: { deepseek: 'tes…mask', doubao: 'db…saved' }, llmProvider: 'deepseek', llmModel: 'deepseek-flash' };
    window.__TAURI_INTERNALS__ = { invoke: async (command, args) => {
      if (command === 'load_voice_chat_settings') return window.modelTestSettings;
      if (command === 'save_voice_chat_settings') {
        window.modelTestSaved = args.input;
        const scope = args.input.llmProvider === 'custom' ? `custom:${args.input.llmBaseUrl.replace(/\/+$/, '')}` : args.input.llmProvider;
        const masked = window.modelTestSettings.llmApiKeyMasks[scope] || '';
        window.modelTestSettings = { ...window.modelTestSettings, ...args.input,
          llmConfigured: Boolean(masked), llmApiKeyMasked: masked };
        return window.modelTestSettings;
      }
      if (command === 'load_device_asr_settings') return { configured: false };
      if (command === 'load_llm_network_settings') return { mode: 'system', proxyUrl: '', hasCertificate: false };
      throw new Error(`Unexpected command: ${command}`);
    } };
    const React = (await import('/node_modules/.vite/deps/react.js')).default;
    const dom = await import('/node_modules/.vite/deps/react-dom_client.js');
    const { default: ApiSettings } = await import('/src/ApiSettings.jsx');
    const { default: BoardButtonPanel } = await import('/src/dashboard/BoardButtonPanel.jsx');
    const { DEFAULT_VOICE_CONFIG } = await import('/src/DeviceDashboard.jsx');
    const css = document.createElement('link'); css.rel = 'stylesheet'; css.href = '/src/styles.css'; document.head.append(css);
    const root = (dom.default || dom).createRoot(document.getElementById('root'));
    window.showButtons = () => root.render(React.createElement(BoardButtonPanel, { runtime: 'esp-p4', voiceConfig: DEFAULT_VOICE_CONFIG, usbConnected: true }));
    root.render(React.createElement(ApiSettings));
  });
  const card = page.locator('.card').filter({ has: page.getByRole('heading', { name: '对话大模型', exact: true }) });
  await card.waitFor();
  const model = card.getByLabel('模型名称 / 接入点 ID', { exact: true });
  await model.waitFor({ state: 'visible' });
  await page.waitForFunction(() => !document.getElementById('api-settings-llm-model').disabled);
  const key = card.locator('#api-settings-llm-key');
  const search = card.getByRole('checkbox', { name: '按需联网检索' });
  assert.equal(await search.isChecked(), true);
  await search.uncheck();
  await card.getByRole('button', { name: '保存', exact: true }).click();
  await page.waitForFunction(() => window.modelTestSaved?.llmWebSearch === false);
  assert.equal(await search.isChecked(), false);
  assert.match(await key.getAttribute('placeholder'), /tes…mask/);
  await key.fill('unsaved-test-key');
  await card.getByRole('radio', { name: '豆包', exact: true }).click();
  assert.equal(await key.inputValue(), '');
  assert.match(await key.getAttribute('placeholder'), /db…saved/);
  assert.match(await card.locator('.api-settings__result').first().innerText(), /已读取该服务/);
  await card.getByRole('radio', { name: 'DeepSeek', exact: true }).click();
  assert.match(await key.getAttribute('placeholder'), /tes…mask/);
  await card.getByRole('radio', { name: 'MiMo', exact: true }).click();
  assert.equal(await key.getAttribute('placeholder'), '粘贴这家模型的 API Key');
  await card.getByRole('radio', { name: 'DeepSeek', exact: true }).click();
  for (const [provider, expected] of [['DeepSeek', 'deepseek-flash'], ['豆包', 'doubao-seed-2-0-lite-260428'], ['MiMo', 'mimo-v2.6-flash']]) {
    await card.getByRole('radio', { name: provider, exact: true }).click();
    assert.equal(await model.inputValue(), expected);
    await model.fill('  account-model-id  ');
    await card.getByRole('radio', { name: provider, exact: true }).click();
    assert.equal(await model.inputValue(), '  account-model-id  ');
    await card.getByRole('button', { name: '保存', exact: true }).click();
    await page.waitForFunction(() => window.modelTestSaved?.llmModel === 'account-model-id');
    assert.equal(await model.inputValue(), 'account-model-id');
    assert.equal(await page.evaluate(() => window.modelTestSaved.llmBaseUrl), null);
    await page.evaluate(() => { window.modelTestSaved = null; });
  }
  await card.getByRole('radio', { name: '自定义', exact: true }).click();
  assert.equal(await model.inputValue(), '');
  await card.getByLabel('接口地址（OpenAI 兼容）', { exact: true }).fill('https://example.invalid/v1');
  await model.fill('custom-model');
  await card.getByRole('button', { name: '保存', exact: true }).click();
  await page.waitForFunction(() => window.modelTestSaved?.llmModel === 'custom-model');
  assert.equal(await page.evaluate(() => window.modelTestSaved.llmBaseUrl), 'https://example.invalid/v1');
  await key.fill('custom-test-key');
  await card.getByLabel('接口地址（OpenAI 兼容）', { exact: true }).fill('https://new.invalid/v1');
  assert.equal(await key.inputValue(), '');
  assert.equal(await key.getAttribute('placeholder'), '粘贴这家模型的 API Key');
  assert.equal(await model.inputValue(), 'custom-model');
  await card.getByRole('radio', { name: 'MiMo', exact: true }).click();
  assert.match(await card.locator('.card__subtitle').innerText(), /不影响 ChatGPT、Claude 等 Agent 的语音输入/);
  assert.equal(await card.locator('.card__body').getByText('此配置用于和宠物聊天', { exact: false }).count(), 0);
  const spacing = await card.locator('.usage-help').evaluate(el => ({
    margin: getComputedStyle(el).margin,
    paddingTop: getComputedStyle(el.parentElement).paddingTop,
    paddingBottom: getComputedStyle(el.parentElement).paddingBottom,
  }));
  assert.equal(spacing.margin, '0px');
  assert.equal(spacing.paddingTop, '12px');
  assert.equal(spacing.paddingBottom, '16px');
  const subtitle = await card.locator('.card__subtitle').evaluate(el => ({height:el.getBoundingClientRect().height,lineHeight:parseFloat(getComputedStyle(el).lineHeight),maxWidth:getComputedStyle(el).maxWidth}));
  assert.equal(subtitle.maxWidth, 'none');
  assert.ok(subtitle.height <= subtitle.lineHeight + 1, 'desktop subtitle should use the full card width on one line');
  if (process.env.PET_TEST_SCREENSHOT) await card.screenshot({ path: process.env.PET_TEST_SCREENSHOT });
  await page.setViewportSize({ width: 820, height: 700 });
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
  await page.evaluate(() => window.showButtons());
  const summary = page.getByText('常用操作 · 按当前配置显示', { exact: true });
  await summary.waitFor();
  assert.equal(await summary.evaluate(el => el.parentElement.open), true);
  await summary.click();
  assert.equal(await summary.evaluate(el => el.parentElement.open), false);
  await summary.click();
  assert.equal(await summary.evaluate(el => el.parentElement.open), true);
  assert.deepEqual(errors, []);
  console.log('PASS settings: editable models for all providers, default switching, trimmed save payloads, custom endpoint, compact layout, common operations.');
} finally { await browser.close(); }
