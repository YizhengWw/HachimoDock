/**
 * [Input] CustomAvatarWizard generation-config source.
 * [Output] Static Node regression coverage for reusable full-generation wizard steps,
 *          shared provider-config-backed Volcano Ark credential readiness, 1.5-first defaults,
 *          activation/manual MP4 upload guidance, product-fit dropdown model names, inline progress support, and fixed reference upload sizing.
 * [Pos] test node in pc/src
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));

function readSource(relativePath) {
  return readFileSync(join(srcDir, relativePath), "utf8");
}

function extractCssRule(css, selector) {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const match = css.match(new RegExp(`${escaped}\\s*\\{(?<body>[^}]+)\\}`));
  assert.ok(match, `Expected to find ${selector} CSS rule`);
  return match.groups.body;
}

test("generation explains portrait authorization and independent video moderation before submission", () => {
  const wizard = readSource("CustomAvatarWizard.jsx");
  assert.match(wizard, /真人照片可能触发 Seedance 的人像或版权审核/);
  assert.match(wizard, /当前客户端暂未接入可信素材库/);
  assert.match(wizard, /图片背景处理成功不代表图生视频审核通过/);
  assert.match(wizard, /自行生成动态形象并导出 MP4/);
  assert.match(wizard, /形象画廊 → 新建自定义形象 → 自定义上传视频/);
  assert.match(wizard, /上传 MP4 替换/);
});

test("full avatar generation exposes reusable step UI for single-state modal regeneration", () => {
  const wizard = readSource("CustomAvatarWizard.jsx");

  assert.match(wizard, /export function AvatarWizardStep1/);
  assert.match(wizard, /export function AvatarWizardStep2/);
  assert.match(wizard, /identityFields = true/);
  assert.match(wizard, /startLabel = "开始生成"/);
  assert.match(wizard, /children/);
  assert.match(wizard, /progress/);
  assert.match(wizard, /ca-inline-progress/);
  assert.match(wizard, /<AvatarWizardStep1/);
  assert.match(wizard, /<AvatarWizardStep2/);
  assert.doesNotMatch(wizard, /<Step1/);
  assert.doesNotMatch(wizard, /<Step2/);
});

test("Volcano generation reads centralized credentials and keeps model selection local", () => {
  const wizard = readSource("CustomAvatarWizard.jsx");
  const providerConfig = readSource("lib/avatar-pipeline/provider-config.js");
  const volcanoProvider = readSource("lib/avatar-pipeline/providers/volcano.js");

  assert.match(volcanoProvider, /doubao-seedance-2-0-260128/);
  assert.doesNotMatch(providerConfig, /doubao-seedance-1-5-pro-251215/);
  assert.match(wizard, /VIDEO_PROVIDERS/);
  assert.match(wizard, /loadProviderConfig/);
  assert.match(wizard, /saveProviderConfig/);
  assert.match(wizard, /VideoModelSelect/);
  assert.match(wizard, /VideoGenerationSettings/);
  assert.doesNotMatch(wizard, /自定义模型名称/);
  assert.match(wizard, /isVolcengine/);
  assert.match(wizard, /!isVolcengine && \(/);
  assert.match(wizard, /providerCredentialsConfigured/);
  assert.match(wizard, /generation-api-status/);
  assert.match(wizard, /打开 API 配置/);
  assert.match(wizard, /请先在 API 配置中保存火山引擎 API Key/);
  assert.doesNotMatch(wizard, /type="password"/);
  assert.doesNotMatch(wizard, /placeholder="输入或选择火山 Ark 视频模型名称"/);
  assert.doesNotMatch(wizard, /请先填写 Base URL 和视频生成模型/);
  assert.doesNotMatch(wizard, /Thinking 模型 endpoint/);
  assert.doesNotMatch(wizard, /providerId === "volcengine" && !thinkingModel\.trim\(\)/);
  assert.doesNotMatch(wizard, /推理接入点 \/ Endpoint/);
  assert.doesNotMatch(wizard, /doubao-seedance-1-0-/);
});

test("reference image upload area keeps the same size before and after preview", () => {
  const wizard = readSource("CustomAvatarWizard.jsx");
  const css = readSource("styles.css");
  const dropzone = extractCssRule(css, ".dropzone");
  const preview = extractCssRule(css, ".dropzone__preview");
  const filename = extractCssRule(css, ".dropzone__filename");

  assert.match(wizard, /className="dropzone__filename"/);
  assert.match(dropzone, /--dropzone-height:\s*160px;/);
  assert.match(dropzone, /height:\s*var\(--dropzone-height\);/);
  assert.match(dropzone, /justify-content:\s*center;/);
  assert.match(dropzone, /overflow:\s*hidden;/);
  assert.match(preview, /width:\s*var\(--dropzone-preview-size\);/);
  assert.match(preview, /height:\s*var\(--dropzone-preview-size\);/);
  assert.match(filename, /width:\s*100%;/);
  assert.match(filename, /min-width:\s*0;/);
  assert.match(filename, /white-space:\s*nowrap;/);
  assert.match(filename, /text-overflow:\s*ellipsis;/);
});
