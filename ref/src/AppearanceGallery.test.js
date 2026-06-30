/**
 * [Input] Appearance gallery source and shared stylesheet.
 * [Output] Static Node test coverage for gallery-only creation/import/detail management,
 *          the source chooser behind 新建自定义形象, direct uploaded-video appearances,
 *          filled card preview media, full Codex import previews, unobstructed gallery cards,
 *          and removal of desktop-pet assignment controls from the gallery.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));

function readSource(fileName) {
  return readFileSync(join(srcDir, fileName), "utf8");
}

function extractCssRule(css, selector) {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const match = css.match(new RegExp(`${escaped}\\s*\\{(?<body>[^}]+)\\}`));
  assert.ok(match, `Expected to find ${selector} CSS rule`);
  return match.groups.body;
}

test("gallery renders inside PageShell with refresh plus a four-source creation chooser", () => {
  const gallery = readSource("AppearanceGallery.jsx");
  const css = readSource("styles.css");
  const actions = extractCssRule(css, ".appearance-gallery-actions");

  assert.match(gallery, /import PageShell from "\.\/shell\/PageShell\.jsx"/);
  assert.match(gallery, /<PageShell\b[\s\S]*title="形象画廊"/);
  assert.match(gallery, /actions=\{\[refreshButton, addAppearanceActions\]\}/);
  assert.match(gallery, /className="appearance-gallery-actions"/);
  assert.match(gallery, /const \[creationModalOpen, setCreationModalOpen\] = useState\(false\);/);
  assert.match(gallery, /onClick=\{\(\) => setCreationModalOpen\(true\)\}/);
  assert.match(gallery, /<CreationSourceModal\b/);
  assert.match(gallery, /onAiGenerate=\{handleAiGenerateSource\}/);
  assert.match(gallery, /onVideoUpload=\{handleVideoUploadSource\}/);
  assert.match(gallery, /onCodexImport=\{handleCodexImportSource\}/);
  assert.match(gallery, /onCommunityImport=\{handleCommunityImportSource\}/);
  assert.match(gallery, /图片AI生成/);
  assert.match(gallery, /自定义上传视频/);
  assert.match(gallery, /Codex 导入/);
  assert.match(gallery, /从社区导入/);
  assert.match(css, /\.creation-source-grid\s*\{/);
  assert.match(css, /\.creation-source-card\s*\{/);
  assert.doesNotMatch(gallery, /function SplitButton\(/);
  assert.doesNotMatch(gallery, /split-button__/);
  assert.match(actions, /display:\s*inline-flex;/);
  assert.match(actions, /flex-wrap:\s*wrap;/);
});

test("gallery can create a custom appearance from an uploaded MP4 state video", () => {
  const gallery = readSource("AppearanceGallery.jsx");
  const css = readSource("styles.css");

  assert.match(gallery, /saveUploadedVideoAppearance/);
  assert.match(gallery, /import \{ FAMILIES \} from "\.\/lib\/avatar-pipeline\/families\.js";/);
  assert.match(gallery, /const \[videoUploadModalOpen, setVideoUploadModalOpen\] = useState\(false\);/);
  assert.match(gallery, /function VideoUploadModal\(/);
  assert.match(gallery, /accept="video\/mp4,\.mp4"/);
  assert.match(gallery, /readFileAsBytes\(videoFile\)/);
  assert.match(gallery, /saveUploadedVideoAppearance\(\{[\s\S]*appearanceName:[\s\S]*family:[\s\S]*videoBytes:/);
  assert.match(gallery, /onOpenDetail\?\.\(record\.id\)/);
  assert.match(gallery, /当前仅支持 MP4 状态视频/);
  assert.match(gallery, /className="video-upload-modal__drop"/);
  assert.match(gallery, /className="video-upload-modal__grid"/);
  assert.match(css, /\.video-upload-modal__grid\s*\{/);
  assert.match(css, /\.video-upload-modal__drop\s*\{/);
});

test("gallery is generation/import/detail management only, not desktop-pet assignment", () => {
  const gallery = readSource("AppearanceGallery.jsx");
  const css = readSource("styles.css");

  assert.doesNotMatch(gallery, /applyDesktopPet/);
  assert.doesNotMatch(gallery, /AppearanceChannelModal/);
  assert.doesNotMatch(gallery, /ChannelSwitchConfirmModal/);
  assert.doesNotMatch(gallery, /setChannelModal/);
  assert.doesNotMatch(gallery, /handleSetDesktopPet/);
  assert.doesNotMatch(gallery, /onSetAsDesktopPet/);
  assert.doesNotMatch(gallery, /desktop-pet-btn/);
  assert.doesNotMatch(gallery, /appearance-card-assigned/);
  assert.doesNotMatch(css, /\.desktop-pet-btn/);
  assert.doesNotMatch(css, /\.appearance-card-assigned/);
  assert.doesNotMatch(css, /\.modal-card--channel/);
  assert.doesNotMatch(css, /\.appearance-channel-progress/);
});

test("gallery does not surface agent binding badges on appearance cards", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  assert.doesNotMatch(gallery, /configuredAgentIdsForAppearance/);
  assert.doesNotMatch(gallery, /configuredAgentIds/);
  assert.doesNotMatch(gallery, /channelLabelForId/);
  assert.doesNotMatch(gallery, /agentAppearanceMap/);
  assert.doesNotMatch(gallery, /enabledAgents/);
  assert.match(gallery, /className="appearance-card-tags"/);
  assert.match(gallery, /isBuiltin \? "内置" : isCodex \? "Codex" : "自定义"/);
});

test("gallery reads only current display state from shared device context", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  assert.match(gallery, /useDeviceContext\(\)/);
  assert.match(gallery, /const \{ currentDisplay \} = useDeviceContext\(\)/);
  assert.match(gallery, /const activeAppearanceId = currentDisplay\.appearance\?\.id \|\| "";/);
  assert.doesNotMatch(gallery, /const\s*\{\s*[^}]*usb[^}]*\}\s*=\s*useDeviceContext\(\)/);
  assert.doesNotMatch(gallery, /invoke\("usb_get_status"\)/);
  assert.doesNotMatch(gallery, /invoke\("usb_scan_devices"\)/);
  assert.doesNotMatch(gallery, /invoke\("check_device_availability"\)/);
});

test("community import starts with the source website before showing two methods", () => {
  const gallery = readSource("AppearanceGallery.jsx");
  const css = readSource("styles.css");
  const intro = extractCssRule(css, ".community-source-intro");
  const methods = extractCssRule(css, ".community-methods");

  assert.match(gallery, /name:\s*"Codex Pets"/);
  assert.match(gallery, /className="community-source-intro"[\s\S]*className="community-source__title">\{src\.name\}[\s\S]*className="community-methods"/);
  assert.match(intro, /border:\s*1px solid var\(--line\);/);
  assert.match(methods, /display:\s*grid;/);
  assert.match(methods, /gap:\s*12px;/);
});

test("gallery card previews fit width while Codex imports show the whole subject", () => {
  const gallery = readSource("AppearanceGallery.jsx");
  const css = readSource("styles.css");
  const appearanceGrid = extractCssRule(css, ".appearance-grid");
  const cardPreviewMedia = extractCssRule(css, ".appearance-card-preview .appearance-channel-preview__media");
  const codexPreviewMedia = extractCssRule(css, ".appearance-card-preview--codex .appearance-channel-preview__media");

  assert.match(appearanceGrid, /grid-template-columns:\s*repeat\(auto-fill,\s*minmax\(240px,\s*1fr\)\);/);
  assert.match(gallery, /appearance-card-preview--codex/);
  assert.match(gallery, /playing=\{previewMedia\.kind === "video"\}/);
  assert.match(cardPreviewMedia, /object-fit:\s*contain/);
  assert.match(cardPreviewMedia, /object-position:\s*center\s+center/);
  assert.match(cardPreviewMedia, /transform-origin:\s*center\s+center;/);
  assert.doesNotMatch(cardPreviewMedia, /transform:\s*scale\(/);
  assert.doesNotMatch(css, /\.appearance-card-preview--codex\s*\{[\s\S]*background:\s*var\(--surface\);/);
  assert.match(codexPreviewMedia, /object-fit:\s*contain/);
  assert.match(codexPreviewMedia, /object-position:\s*center\s+center/);
  assert.match(codexPreviewMedia, /max-width:\s*64%;/);
  assert.match(codexPreviewMedia, /max-height:\s*72%;/);
  assert.match(codexPreviewMedia, /align-self:\s*center;/);
  assert.match(codexPreviewMedia, /justify-self:\s*center;/);
  assert.match(codexPreviewMedia, /transform-origin:\s*center\s+center;/);
  assert.doesNotMatch(codexPreviewMedia, /object-position:\s*center\s+top;/);
  assert.match(css, /\.appearance-card-preview\s*\{[\s\S]*height:\s*clamp\(120px,\s*11vw,\s*170px\);/);
  assert.match(css, /\.appearance-card-preview\s*\{[\s\S]*padding:\s*12px\s+18px\s+16px;/);
  assert.match(css, /\.appearance-card-preview\s*\{[\s\S]*background:\s*#000;/);
  assert.match(css, /\.appearance-card--clickable:hover \.appearance-channel-preview__media,[\s\S]*transform:\s*none;/);
});

test("codex import rows use backend-generated preview images instead of local file backgrounds", () => {
  const gallery = readSource("AppearanceGallery.jsx");
  const css = readSource("styles.css");
  const codexPreview = extractCssRule(css, ".codex-pet-preview");

  assert.doesNotMatch(gallery, /convertFileSrc/);
  assert.match(gallery, /function codexPetPreviewSrc/);
  assert.match(gallery, /className="codex-pet-preview"/);
  assert.match(gallery, /className="codex-pet-preview__image"/);
  assert.match(gallery, /src=\{previewSrc\}/);
  assert.match(codexPreview, /overflow:\s*hidden;/);
  assert.doesNotMatch(codexPreview, /background-size:\s*800%\s+900%;/);
  assert.match(css, /\.codex-pet-preview__image\s*\{[\s\S]*object-fit:\s*contain;/);
});

test("gallery cards surface the active appearance as a read-only badge", () => {
  const gallery = readSource("AppearanceGallery.jsx");
  const css = readSource("styles.css");

  assert.doesNotMatch(gallery, /function CustomCard\(/);
  assert.match(gallery, /function AppearanceCard\(/);
  assert.match(gallery, /<AppearanceCard\b/);
  assert.match(gallery, /isActive=\{row\.id === activeAppearanceId\}/);
  assert.match(gallery, /appearance-card__badge--active/);
  assert.match(gallery, /<CheckCircle2 size=\{12\} \/> 使用中/);
  assert.match(gallery, /<AppearancePreview[\s\S]*<\/div>\s*<div className="appearance-card-body">[\s\S]*className="appearance-card-tags"/);

  const activeCard = extractCssRule(css, ".appearance-card.is-active");
  assert.match(activeCard, /border-color:\s*var\(--accent\);/);
});

test("gallery cards are compact now that desktop-pet actions are removed", () => {
  const css = readSource("styles.css");
  const card = extractCssRule(css, ".appearance-card");
  const body = extractCssRule(css, ".appearance-card-body");
  const main = extractCssRule(css, ".appearance-card-main");

  assert.match(card, /height:\s*100%;/);
  assert.match(body, /flex:\s*1;/);
  assert.match(body, /min-height:\s*144px;/);
  assert.match(main, /flex:\s*1;/);
});

test("gallery uses cached appearance and Codex scans with explicit force refreshes", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  assert.match(gallery, /getCachedAppearances/);
  assert.match(gallery, /listAppearances\(\{ force \}\)/);
  assert.match(gallery, /reload\(\{ force: true \}\)/);
  assert.match(gallery, /from "\.\/lib\/codex-pets-client\.js"/);
  assert.match(gallery, /listCodexPets\(\)/);
  assert.match(gallery, /listCodexPets\(\{ force: true \}\)/);
  assert.doesNotMatch(gallery, /invoke\("list_codex_pets"\)/);
  assert.doesNotMatch(gallery, /invoke\("install_codex_community_pet"/);
  assert.doesNotMatch(gallery, /saveAgentAppearanceMap\(nextMap\)/);
});

test("running task card is wrapped in a shell Card and only rendered while a task is in flight", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  assert.match(gallery, /import Card from "\.\/shell\/Card\.jsx"/);
  assert.match(
    gallery,
    /\{taskRunning && \(\s*<Card>\s*<RunningTaskCard/,
  );
  assert.match(gallery, /const taskRunning = task\?\.status === "running";/);
});
