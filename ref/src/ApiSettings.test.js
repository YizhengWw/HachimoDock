/**
 * [Input] ApiSettings page, App routing, feature pages, and shared stylesheet source.
 * [Output] Static regression coverage for centralized API-key ownership, prompt-free macOS private-file disclosure, sidebar routing, feature-page status links, and responsive settings layout.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));
const readSource = (file) => readFileSync(join(srcDir, file), "utf8");

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
  assert.match(source, /macOS 不使用钥匙串/);
  assert.match(source, /仅由当前用户读取，不额外加密/);
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
