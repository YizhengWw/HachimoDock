/**
 * [Input] ApiSettings page, App routing, feature pages, and shared stylesheet source.
 * [Output] Editable LLM model/default regression coverage plus centralized API-key ownership, speech key guide, saved-ASR broadcasts, routing and responsive layout.
 * [Pos] test node in pc/src
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { LLM_PRESETS, DEFAULT_LLM_PROVIDER, selectLlmProvider, changeLlmEndpoint, describeLlmPreset } from "./lib/api-configuration.js";

const srcDir = dirname(fileURLToPath(import.meta.url));
const readSource = (file) => readFileSync(join(srcDir, file), "utf8");

test("fresh configuration selects Doubao and search toggle is saved", () => {
  assert.equal(DEFAULT_LLM_PROVIDER,"doubao");
  const page=readSource("ApiSettings.jsx");
  assert.match(page,/llmProvider: DEFAULT_LLM_PROVIDER/);
  assert.match(page,/llmWebSearch: voiceChat.llmWebSearch/);
  assert.match(page,/llmWebSearch: status\?\.llmWebSearch !== false/);
});

test("switching chat providers clears draft, masked key and configured state", () => {
  const providers = [...LLM_PRESETS.map(p => p.id), "custom"];
  for (const from of providers) for (const to of providers) {
    const current = { llmProvider: from, llmApiKey: "test-draft", llmApiKeyMasked: "tes…mask",
      llmConfigured: true, llmTone: "success", llmMessage: "已配置" };
    const next = selectLlmProvider(current, to);
    if (from === to) { assert.equal(next, current); continue; }
    assert.equal(next.llmApiKey, ""); assert.equal(next.llmApiKeyMasked, "");
    assert.equal(next.llmConfigured, false); assert.equal(next.llmTone, "muted");
    assert.match(next.llmMessage, /不会沿用/);
    assert.equal(current.llmApiKey, "test-draft");
  }
});

test("custom endpoint changes invalidate credentials, formatting changes do not", () => {
  const current = { llmProvider: "custom", llmBaseUrl: "https://example.invalid/v1",
    llmApiKey: "test-draft", llmApiKeyMasked: "tes…mask", llmConfigured: true };
  for (const url of ["https://another.invalid/v1", "https://example.invalid/v2", ""]) {
    const next = changeLlmEndpoint(current, url);
    assert.equal(next.llmApiKey, ""); assert.equal(next.llmApiKeyMasked, "");
    assert.equal(next.llmConfigured, false); assert.equal(next.llmBaseUrl, url);
  }
  assert.equal(changeLlmEndpoint(current, " https://example.invalid/v1/ ").llmApiKey, "test-draft");
});

test("provider and custom endpoint switches restore only their own saved masks", () => {
  const masks = { deepseek: "ds…0001", doubao: "db…0002", mimo: "mi…0003",
    "custom:https://one.invalid/v1": "one…0004", "custom:https://two.invalid/v1": "two…0005" };
  const current = { llmProvider: "deepseek", llmApiKey: "unsaved-test-draft", llmApiKeyMasks: masks,
    llmCustomBaseUrl: "https://one.invalid/v1" };
  for (const provider of ["doubao", "mimo"]) {
    const next = selectLlmProvider(current, provider);
    assert.equal(next.llmApiKey, ""); assert.equal(next.llmApiKeyMasked, masks[provider]);
    assert.equal(next.llmConfigured, true);
    assert.equal(selectLlmProvider(next, "deepseek").llmApiKeyMasked, masks.deepseek);
  }
  const custom = selectLlmProvider(current, "custom");
  assert.equal(custom.llmBaseUrl, "https://one.invalid/v1");
  assert.equal(custom.llmApiKeyMasked, "one…0004");
  assert.equal(changeLlmEndpoint(custom, "https://two.invalid/v1/").llmApiKeyMasked, "two…0005");
  assert.equal(changeLlmEndpoint(custom, "https://unknown.invalid").llmConfigured, false);
});

test("chat model names are editable for presets and provider changes use their own defaults", () => {
  assert.deepEqual(LLM_PRESETS.map(({ model }) => model), ["deepseek-flash", "doubao-seed-2-0-lite-260428", "mimo-v2.6-flash"]);
  const current = { llmProvider: "deepseek", llmModel: "my-deepseek-model", llmBaseUrl: "old", llmApiKey: "" };
  assert.equal(selectLlmProvider(current, "deepseek"), current);
  for (const preset of LLM_PRESETS.slice(1)) {
    const next = selectLlmProvider(current, preset.id);
    assert.equal(next.llmModel, preset.model);
    assert.equal(next.llmBaseUrl, preset.baseUrl);
  }
  assert.equal(selectLlmProvider(current, "custom").llmModel, "");
  assert.match(describeLlmPreset("mimo", "my-model"), /MiMo · my-model/);
  assert.match(describeLlmPreset("mimo", "  "), /mimo-v2\.6/);
  const page = readSource("ApiSettings.jsx");
  assert.match(page, /llmModel: voiceChat\.llmModel\.trim\(\)/);
  assert.doesNotMatch(page, /llmModel: custom \?/);
  assert.match(page, /\)\}\s*<label[^>]*htmlFor="api-settings-llm-model"/);
});

test("LLM scope explanation belongs to header and usage spacing is page-scoped", () => {
  const page=readSource("ApiSettings.jsx");
  assert.match(page, /title="对话大模型" subtitle="[^"]*不影响 ChatGPT、Claude 等 Agent 的语音输入/);
  assert.doesNotMatch(page, /<p[^>]*>此配置用于和宠物聊天/);
  assert.match(page, /className="api-settings-page"/);
  assert.match(readSource("styles.css"), /\.api-settings-page \.usage-help \{ margin: 0; \}/);
  assert.match(readSource("styles.css"), /\.api-settings-page \.card__subtitle \{ max-width: none;/);
});

test("API settings owns voice and generation credential inputs", () => {
  const source = readSource("ApiSettings.jsx");

  assert.match(source, /title="API 配置"/);
  assert.match(source, /load_device_asr_settings/);
  assert.match(source, /save_device_asr_settings/);
  assert.match(source, /test_device_asr_settings/);
  assert.match(source, /VIDEO_PROVIDERS\.map/);
  assert.match(source, /Access Key/);
  assert.match(source, /Secret Key/);
  assert.match(source, /type=\{showSecrets \? "text" : "password"\}/);
  assert.match(source, /saveProviderConfig/);
  assert.match(source, /emitApiConfigurationUpdated/);
  assert.doesNotMatch(source, /macOS 不使用钥匙串|仅由当前用户读取，不额外加密|无需选择模型版本|火山引擎豆包语音（识别 \+ 合成）/);
  assert.doesNotMatch(source, /2628951/);
  assert.match(source, /title="语音识别与合成"\s+subtitle=\{\([\s\S]*?识别与合成共用一个 API Key[\s\S]*?href="https:\/\/docs\.volcengine\.com\/docs\/DoubaoVoice\/APIKeyUsage\?lang=zh"[\s\S]*?Key 获取方式[\s\S]*?actions=\{/);
  assert.match(source, /<details>[\s\S]*ASR 2\.0 和 TTS 2\.0/);
  assert.match(source, /https:\/\/ark\.volcengine\.com\/model\/detail\?name=doubao-seedance-2-0-mini/);
  assert.match(source, /provider\.id === "volcengine"/);
  assert.equal((source.match(/Key 获取方式/g) || []).length, 2);
  assert.equal((source.match(/target="_blank"/g) || []).length, 2);
  assert.equal((source.match(/rel="noreferrer"/g) || []).length, 2);
});

test("saved ASR credentials are broadcast before the optional cloud probe", () => {
  const source = readSource("ApiSettings.jsx");
  const saveAndTest = source.match(/const saveAndTestAsr = async \(\) => \{[\s\S]*?\n  \};/);
  assert.ok(saveAndTest, "expected saveAndTestAsr");

  const saveIndex = saveAndTest[0].indexOf('invoke("save_device_asr_settings"');
  const broadcastIndex = saveAndTest[0].indexOf("emitApiConfigurationUpdated({");
  const probeIndex = saveAndTest[0].indexOf('invoke("test_device_asr_settings"');
  assert.ok(saveIndex !== -1 && broadcastIndex !== -1 && probeIndex !== -1);
  assert.ok(saveIndex < broadcastIndex, "persist credentials before broadcasting");
  assert.ok(broadcastIndex < probeIndex, "broadcast must not wait for the probe");
  assert.match(saveAndTest[0], /providerId: "volcengine-asr"/);
  assert.match(saveAndTest[0], /configured: saved\?\.configured === true/);
});

test("app exposes API configuration as a first-level sidebar page", () => {
  const app = readSource("App.jsx");

  assert.match(app, /import ApiSettings from "\.\/ApiSettings"/);
  assert.match(app, /activeTab = view === "api"/);
  assert.match(app, /title="API 配置"/);
  assert.match(app, /view === "api"[\s\S]*<ApiSettings/);
  assert.match(app, /onOpenApiSettings/);
});

test("feature pages no longer render API-key inputs", () => {
  const wizard = readSource("CustomAvatarWizard.jsx");
  const detail = readSource("AppearanceDetail.jsx");
  const voice = readSource("dashboard/VoiceAssistantPanel.jsx");

  assert.doesNotMatch(wizard, /type="password"/);
  assert.doesNotMatch(detail, /type="password"/);
  assert.doesNotMatch(voice, /type="password"/);
  assert.match(wizard, /打开 API 配置/);
  assert.match(voice, /前往 API 配置/);
});

test("API settings uses a responsive provider grid", () => {
  const css = readSource("styles.css");

  assert.match(css, /\.api-settings__provider-grid\s*\{/);
  assert.match(css, /@media \(max-width: 900px\)[\s\S]*\.api-settings__provider-grid/);
  assert.match(css, /@media \(max-width: 620px\)[\s\S]*\.api-settings__form--asr/);
});
