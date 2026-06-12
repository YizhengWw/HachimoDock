/**
 * [Input] Appearance persistence adapter source.
 * [Output] Static Node coverage that keeps manifest listing behind an explicit force-refreshable cache with synchronous cached reads and persists built-in/custom per-family WAV cue configuration.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const libDir = dirname(fileURLToPath(import.meta.url));

test("listAppearances is cached and storage mutations invalidate the cache", () => {
  const source = readFileSync(join(libDir, "appearance-store.js"), "utf8");

  assert.match(source, /import \{ createAsyncCache \} from "\.\/client-cache\.js";/);
  assert.match(source, /const appearanceListCache = createAsyncCache\(\{ ttlMs: APPEARANCE_LIST_CACHE_TTL_MS \}\);/);
  assert.match(source, /export async function listAppearances\(\{ force = false \} = \{\}\)/);
  assert.match(source, /return appearanceListCache\.get\(readAppearanceRecords, \{ force \}\);/);
  assert.match(source, /export function getCachedAppearances\(\)/);
  assert.match(source, /return appearanceListCache\.peek\(\);/);
  assert.match(source, /export function invalidateAppearanceCache\(\)/);
  assert.match(source, /appearanceListCache\.invalidate\(\);/);
  assert.match(source, /export async function saveAppearance[\s\S]*invalidateAppearanceCache\(\);/);
  assert.match(source, /export async function deleteAppearance[\s\S]*invalidateAppearanceCache\(\);/);
  assert.match(source, /export async function replaceFamilyVideo[\s\S]*invalidateAppearanceCache\(\);/);
});

test("appearance store persists and clears per-family WAV cues in the manifest", () => {
  const source = readFileSync(join(libDir, "appearance-store.js"), "utf8");

  assert.match(source, /BUILTIN_AUDIO_OVERRIDE_FILE = "audio-overrides\.json"/);
  assert.match(source, /BUILTIN_AUDIO_OVERRIDE_DIR = `\$\{ROOT_DIR\}\/\$\{BUILTIN_TERRIER_APPEARANCE_ID\}\/audio-overrides`/);
  assert.match(source, /if \(appearanceId === BUILTIN_TERRIER_APPEARANCE_ID\)/);
  assert.match(source, /defaultAudioCueSrcForFamily/);
  assert.match(source, /export async function replaceFamilyAudioCue/);
  assert.match(source, /const audioRel = `\$\{dir\}\/\$\{VIDEO_DIR\}\/\$\{family\}\.wav`;/);
  assert.match(source, /await writeFile\(audioRel, audioBytes, \{ baseDir: BaseDirectory\.AppLocalData \}\);/);
  assert.match(source, /manifest\.families\[idx\] = \{ \.\.\.manifest\.families\[idx\], audioPath: audioRel \};/);
  assert.match(source, /export async function removeFamilyAudioCue/);
  assert.match(source, /delete manifest\.families\[idx\]\.audioPath;/);
  assert.match(source, /export async function readAudioAsBlobUrl\(audioPath\)/);
  assert.match(source, /return readAppearanceFileAsBlobUrl\(audioPath, "audio\/wav"\);/);
});
