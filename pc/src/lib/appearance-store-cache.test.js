/**
 * [Input] Appearance persistence adapter source.
 * [Output] Static Node coverage that keeps manifest listing behind an explicit force-refreshable cache with synchronous cached reads,
 *          persists direct uploaded-video appearances, and persists built-in/custom per-family WAV cue configuration.
 * [Pos] test node in pc/src/lib
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const libDir = dirname(fileURLToPath(import.meta.url));

test("listAppearances is cached and storage mutations invalidate the cache", () => {
  const source = readFileSync(join(libDir, "appearance-store.js"), "utf8");

  assert.match(source, /import \{ FAMILIES \} from "\.\/avatar-pipeline\/families\.js";/);
  assert.match(source, /import \{ createAsyncCache \} from "\.\/client-cache\.js";/);
  assert.match(source, /const appearanceListCache = createAsyncCache\(\{ ttlMs: APPEARANCE_LIST_CACHE_TTL_MS \}\);/);
  assert.match(source, /export async function listAppearances\(\{ force = false \} = \{\}\)/);
  assert.match(source, /return appearanceListCache\.get\(readAppearanceRecords, \{ force \}\);/);
  assert.match(source, /export function getCachedAppearances\(\)/);
  assert.match(source, /return appearanceListCache\.peek\(\);/);
  assert.match(source, /export function invalidateAppearanceCache\(\)/);
  assert.match(source, /appearanceListCache\.invalidate\(\);/);
  assert.match(source, /export async function saveAppearance[\s\S]*invalidateAppearanceCache\(\);/);
  assert.match(source, /export async function saveUploadedVideoAppearance[\s\S]*invalidateAppearanceCache\(\);/);
  assert.match(source, /export async function deleteAppearance[\s\S]*invalidateAppearanceCache\(\);/);
  assert.match(source, /export async function replaceFamilyVideo[\s\S]*invalidateAppearanceCache\(\);/);
});

test("appearance store can persist an uploaded MP4 as a custom appearance with replaceable state slots", () => {
  const source = readFileSync(join(libDir, "appearance-store.js"), "utf8");

  assert.match(source, /export async function saveUploadedVideoAppearance\(input\)/);
  assert.match(source, /const selectedFamily = input\.family \|\| FAMILIES\[0\]\?\.family \|\| "working";/);
  assert.match(source, /const videoRel = `\$\{dir\}\/\$\{VIDEO_DIR\}\/\$\{selectedFamily\}\.mp4`;/);
  assert.match(source, /await writeFile\(videoRel, input\.videoBytes, \{ baseDir: BaseDirectory\.AppLocalData \}\);/);
  assert.match(source, /type: "custom"/);
  assert.match(source, /provider: "local-upload"/);
  assert.match(source, /model: "uploaded-mp4"/);
  assert.match(source, /families: buildUploadedVideoFamilies\(selectedFamily, videoRel, input\.originalFilename\)/);
  assert.match(source, /function buildUploadedVideoFamilies/);
  assert.match(source, /FAMILIES\.map\(\(definition\) =>/);
  assert.match(source, /ok: definition\.family === selectedFamily/);
  assert.match(source, /error: "尚未上传状态视频"/);
});

test("legacy audio entry points save global cues without preparing a video pack", () => {
 const source=readFileSync(join(libDir,"appearance-store.js"),"utf8");
 const edits=source.slice(source.indexOf('export async function replaceFamilyAudioCue'),source.indexOf('/**',source.indexOf('export async function removeFamilyAudioCue')));
 assert.match(edits,/saveGlobalAudioCue/);assert.match(edits,/removeGlobalAudioCue/);
 assert.doesNotMatch(edits,/prepareP4Appearance|manifest|writeFile|videos/);
});
