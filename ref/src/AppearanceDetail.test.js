/**
 * [Input] Appearance detail source and shared stylesheet.
 * [Output] Static Node test coverage for detail navigation, guarded deletion,
 *          task-focused preview-first layout, configurable state WAV cues,
 *          direct per-state MP4 replacement, closable background single-state generation
 *          that replaces client videos before manual board sync, flat modal chrome,
 *          inline progress, built-in non-deletable records, generated-only materials, and codex pet source labels.
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

function extractModalFooter(source) {
  const match = source.match(/<div className="single-state-regenerate-modal__footer">(?<body>[\s\S]*?)<\/div>/);
  assert.ok(match, "Expected to find single-state regenerate modal footer");
  return match.groups.body;
}

test("appearance detail stays focused on preview, deletion, and gallery navigation", () => {
  const source = readSource("AppearanceDetail.jsx");
  const css = readSource("styles.css");

  assert.match(source, /返回宠物图册/);
  assert.match(source, /删除形象/);
  assert.doesNotMatch(source, /应用到设备/);
  assert.doesNotMatch(source, /detail-apply-/);
  assert.doesNotMatch(css, /detail-apply-/);
});

test("appearance detail main preview uses the resilient shared media preview", () => {
  const source = readSource("AppearanceDetail.jsx");
  const css = readSource("styles.css");

  assert.match(source, /const activePreviewMedia = activeRecord\?\.ok/);
  assert.match(source, /mediaFromFamily\(activeRecord, record\)/);
  assert.match(source, /mediaFromSourcePreview\(record\)/);
  assert.match(source, /mediaFromSourceImage\(record\)/);
  assert.match(source, /<AppearancePreview[\s\S]*className="detail-media__video"[\s\S]*playing/);
  assert.doesNotMatch(source, /const \[videoUrl/);
  assert.doesNotMatch(source, /readVideoAsBlobUrl/);
  assert.match(css, /\.detail-media__video\s*\{[\s\S]*height: clamp\(220px, 34vw, 320px\);/);
});

test("appearance detail guards deletion with progress and error feedback", () => {
  const source = readSource("AppearanceDetail.jsx");

  assert.match(source, /const \[deleteState, setDeleteState\] = useState\("idle"\);/);
  assert.match(source, /const \[deleteError, setDeleteError\] = useState\(""\);/);
  assert.match(source, /await deleteAppearance\(record\.id\);/);
  assert.match(source, /setDeleteState\("deleting"\);/);
  assert.match(source, /删除中…/);
  assert.match(source, /删除形象失败/);
});

test("appearance detail lets users configure a WAV cue for each generated state", () => {
  const source = readSource("AppearanceDetail.jsx");
  const css = readSource("styles.css");

  assert.match(source, /replaceFamilyAudioCue/);
  assert.match(source, /removeFamilyAudioCue/);
  assert.match(source, /readAudioAsBlobUrl/);
  assert.match(source, /compressWavFileForBoard/);
  assert.match(source, /const \[audioUrl, setAudioUrl\] = useState\(""\);/);
  assert.match(source, /accept="audio\/wav,audio\/x-wav,\.wav"/);
  assert.match(source, /状态提示音/);
  assert.match(source, /上传 WAV/);
  assert.match(source, /移除提示音/);
  assert.match(source, /const canEditAudio = activeRecord\?\.ok;/);
  assert.doesNotMatch(source, /const canEditAudio = !isBuiltin && activeRecord\?\.ok;/);
  assert.match(source, /const audioFamilyNames = useMemo/);
  assert.match(source, /当前状态 \{activeRecord\.family\} 还没有提示音/);
  assert.ok(source.includes('已有提示音: ${audioFamilyNames.join(" / ")}'));
  assert.match(source, /familyRecord\.audioPath \|\| familyRecord\.audioSrc/);
  assert.match(source, /const audioBytes = await compressWavFileForBoard\(file\);/);
  assert.match(css, /\.detail-audio-config\s*\{/);
  assert.match(css, /\.state-card__sound\s*\{/);
});

test("appearance detail lets users upload an MP4 to replace the selected state video", () => {
  const source = readSource("AppearanceDetail.jsx");
  const css = readSource("styles.css");

  assert.match(source, /const \[stateVideoState, setStateVideoState\] = useState\("idle"\);/);
  assert.match(source, /const \[stateVideoMessage, setStateVideoMessage\] = useState\(""\);/);
  assert.match(source, /function isMp4VideoFile/);
  assert.match(source, /readFileAsBytes\(file\)/);
  assert.match(source, /handleStateVideoFileChange/);
  assert.match(source, /accept="video\/mp4,\.mp4"/);
  assert.match(source, /状态视频/);
  assert.match(source, /上传 MP4 替换/);
  assert.match(source, /replaceFamilyVideo\(\{[\s\S]*appearanceId: record\.id,[\s\S]*family: activeFamily,[\s\S]*videoBytes/);
  assert.match(source, /已上传并替换 \$\{activeFamily\} 状态视频。需要在设备生效时，点击“替换到板端”。/);
  assert.match(source, /setSingleStateStatus\("success"\);/);
  assert.match(source, /detail-side-section detail-side-section--video/);
  assert.match(css, /\.detail-state-video-upload\s*\{/);
  assert.match(css, /\.detail-state-video-upload__actions\s*\{/);
});

test("appearance detail uses a preview-first workspace with controls beside it", () => {
  const source = readSource("AppearanceDetail.jsx");
  const css = readSource("styles.css");
  const audioRule = extractCssRule(css, ".detail-audio-config");

  assert.match(source, /detail-workspace/);
  assert.match(source, /detail-preview-panel/);
  assert.match(source, /detail-control-panel/);
  assert.match(source, /detail-context-drawer/);
  assert.match(source, /detail-state-rail/);
  assert.match(source, /detail-summary-card/);
  assert.match(source, /detail-side-section detail-side-section--audio/);
  assert.match(source, /detail-side-section detail-side-section--regenerate/);
  assert.match(source, /detail-summary-card__description/);
  assert.match(source, /detail-summary-card__meta/);
  assert.match(css, /\.detail-workspace\s*\{[\s\S]*grid-template-columns:\s*minmax\(480px,\s*1fr\) minmax\(340px,\s*420px\);/);
  assert.match(css, /\.detail-control-panel\s*\{[\s\S]*position:\s*sticky;/);
  assert.match(css, /\.detail-context-drawer\s*\{[\s\S]*margin-top:\s*16px;/);
  assert.match(css, /\.detail-state-rail\s*\{[\s\S]*grid-auto-flow:\s*column;/);
  assert.doesNotMatch(css, /\.detail-meta\s*\{/);
  assert.match(css, /\.detail-summary-card\s*\{/);
  assert.match(css, /\.detail-side-section\s*\{[\s\S]*padding:\s*16px;/);
  assert.doesNotMatch(audioRule, /padding-top:\s*12px;/);
});

test("appearance detail persists audio OTA reminders and exposes the sound downlink action", () => {
  const source = readSource("AppearanceDetail.jsx");
  const css = readSource("styles.css");

  assert.match(source, /AUDIO_SYNC_DIRTY_PREFIX/);
  assert.match(source, /readAudioSyncDirty/);
  assert.match(source, /writeAudioSyncDirty/);
  assert.match(source, /markAudioSyncDirty/);
  assert.match(source, /usb_sync_appearance/);
  assert.match(source, /下发音效/);
  assert.match(source, /板端音效 OTA 通道/);
  assert.match(source, /已更新 \$\{activeFamily\} 的提示音，请通过音效通道下发到设备。/);
  assert.match(source, /已移除 \$\{activeFamily\} 的提示音，请通过音效通道下发到设备。/);
  assert.match(source, /detail-audio-sync-btn/);
  assert.match(source, /detail-audio-sync-message/);
  assert.match(css, /\.detail-audio-sync-btn\s*\{/);
  assert.match(css, /\.detail-audio-sync-message\s*\{/);
});

test("appearance detail regenerates one selected state locally before manual board sync", () => {
  const source = readSource("AppearanceDetail.jsx");
  const wizard = readSource("CustomAvatarWizard.jsx");
  const css = readSource("styles.css");
  const flatModalCardRule = extractCssRule(css, ".single-state-regenerate-modal .ca-card");
  const modalFooter = extractModalFooter(source);

  assert.match(source, /AvatarWizardStep1/);
  assert.match(source, /AvatarWizardStep2/);
  assert.match(source, /runSingleFamilyVideo/);
  assert.match(source, /replaceFamilyVideo/);
  assert.match(source, /saveProviderConfig/);
  assert.match(source, /handleSingleStateGenerate/);
  assert.match(source, /handleSyncSingleStateToDevice/);
  assert.match(wizard, /accept="image\/png,image\/jpeg,image\/webp,image\/gif,image\/\*"/);
  assert.match(source, /单状态重生成/);
  assert.match(source, /重新生成当前状态/);
  assert.match(source, /singleStateDialogOpen/);
  assert.match(source, /single-state-regenerate-modal/);
  assert.match(source, /完成后只替换客户端视频文件/);
  assert.doesNotMatch(source, /完成后会先替换客户端素材，再由“替换到板端”下发/);
  assert.match(source, /<AvatarWizardStep1[\s\S]*identityFields=\{false\}/);
  assert.match(source, /<AvatarWizardStep2[\s\S]*startLabel="生成并替换客户端视频"/);
  assert.match(source, /生成并替换客户端视频/);
  assert.match(source, /替换到板端/);
  assert.match(source, /usb_sync_appearance/);
  assert.doesNotMatch(modalFooter, /handleSyncSingleStateToDevice/);
  assert.doesNotMatch(modalFooter, /替换到板端/);
  assert.doesNotMatch(modalFooter, /UploadCloud/);
  assert.match(source, /replaceFamilyVideo\(\{[\s\S]*appearanceId: record\.id,[\s\S]*family: activeRecord\.family/);
  assert.match(source, /runSingleFamilyVideo\(\{[\s\S]*family: activeRecord\.family/);
  assert.match(source, /singleStateProgressFromPipeline/);
  assert.match(source, /singleStateProgress/);
  assert.match(source, /progress=\{[\s\S]*singleStateProgress/);
  assert.match(css, /\.detail-state-regenerate-entry\s*\{/);
  // The 重新生成当前状态 CTA now shares the quiet btn-ghost style with the
  // 上传 MP4 / 上传 WAV buttons — unified, no bespoke accent button.
  assert.match(
    source,
    /className="btn-ghost"[\s\S]{0,160}onClick=\{handleOpenSingleStateDialog\}/,
  );
  assert.doesNotMatch(source, /detail-state-regenerate-entry__cta/);
  assert.doesNotMatch(css, /detail-state-regenerate-entry__cta/);
  assert.match(css, /\.ca-inline-progress\s*\{/);
  assert.match(css, /\.ca-inline-progress__bar span\s*\{/);
  assert.match(css, /\.single-state-regenerate-modal\s*\{/);
  assert.match(flatModalCardRule, /border:\s*0;/);
  assert.match(flatModalCardRule, /box-shadow:\s*none;/);
  assert.match(flatModalCardRule, /background:\s*transparent;/);
  assert.match(flatModalCardRule, /padding:\s*0;/);
  assert.doesNotMatch(source, /detail-state-regenerate__preview/);
  assert.doesNotMatch(css, /\.detail-state-regenerate__preview\s*\{/);
});

test("single-state generation can keep running after the modal is closed", () => {
  const source = readSource("AppearanceDetail.jsx");

  assert.match(source, /const handleCloseSingleStateDialog = useCallback\(\(\) => \{\s*setSingleStateDialogOpen\(false\);\s*\}, \[\]\);/);
  assert.doesNotMatch(source, /if \(singleStateStatus === "generating" \|\| singleStateStatus === "syncing"\) return;/);
  assert.doesNotMatch(source, /aria-label="关闭单状态重生成"[\s\S]{0,160}disabled=\{singleStateBusy\}/);
  assert.match(source, /正在生成 \$\{activeRecord\.family\} 状态素材/);
  assert.match(source, /setSingleStateStatus\("success"\);/);
  assert.match(source, /const successMessage = `已替换 \$\{activeRecord\.family\} 状态视频文件，已保存到客户端。`;/);
  assert.match(source, /setSingleStateMessage\(successMessage\);/);
});

test("single-state replacement keeps the board-sync action available after updating the prompt", () => {
  const source = readSource("AppearanceDetail.jsx");

  assert.match(source, /setSingleStatePrompt\(activeRecord\?\.prompt \|\| ""\);/);
  assert.match(source, /\}, \[activeRecord\?\.family, record\?\.id\]\);/);
  assert.doesNotMatch(source, /\}, \[activeRecord\?\.family, activeRecord\?\.prompt, record\?\.id\]\);/);
});

test("appearance detail keeps built-in appearances non-deletable and normalizes source labels", () => {
  const source = readSource("AppearanceDetail.jsx");

  assert.match(source, /const isBuiltin = record\.type === "builtin";/);
  assert.match(source, /appearanceSourceLabel/);
  assert.match(source, /record\.type === "codex-import"/);
  assert.match(source, /"codex pet"/);
  assert.match(source, /"内置形象"/);
});

test("appearance detail renders every known state so missing videos can be replaced", () => {
  const source = readSource("AppearanceDetail.jsx");

  assert.match(source, /const stateFamilyRecords = useMemo\(/);
  assert.match(source, /FAMILIES\.map\(\(definition\) =>/);
  assert.match(source, /stateFamilyRecords\.map\(\(familyRecord\) =>/);
  assert.match(source, /全部状态/);
  assert.match(source, /\{generatedFamilies\.length\}\/\{stateFamilyRecords\.length\} 个已有素材/);
  assert.match(source, /familyRecord\.ok \? "已生成" : "可上传替换"/);
});
