/**
 * [Input] Appearance detail source and shared stylesheet.
 * [Output] Static Node test coverage for detail navigation, guarded deletion,
 *          configurable state WAV cues, editable built-in audio overrides, built-in non-deletable records, generated-only materials,
 *          and codex pet source labels.
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

test("appearance detail keeps built-in appearances non-deletable and normalizes source labels", () => {
  const source = readSource("AppearanceDetail.jsx");

  assert.match(source, /const isBuiltin = record\.type === "builtin";/);
  assert.match(source, /appearanceSourceLabel/);
  assert.match(source, /record\.type === "codex-import"/);
  assert.match(source, /"codex pet"/);
  assert.match(source, /"内置形象"/);
});

test("appearance detail only renders successfully generated family materials", () => {
  const source = readSource("AppearanceDetail.jsx");

  assert.match(source, /const generatedFamilies = useMemo\(/);
  assert.match(source, /record\?\.families\?\.filter\(\(family\) => family\.ok\) \|\| \[\]/);
  assert.match(source, /generatedFamilies\.map\(\(familyRecord\) =>/);
  assert.match(source, /全部素材（\{generatedFamilies\.length\}）/);
});
