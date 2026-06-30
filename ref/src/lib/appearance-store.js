/**
 * [Input] pipeline output or direct uploaded video bytes plus per-family manifest metadata.
 * [Output] cached local filesystem persistence, synchronous cached list reads, direct uploaded-video appearances,
 *          built-in appearance fallback/override merging, source-preview/audio cue asset URLs, and blob URL readers.
 * [Pos] lib node in ref/src/lib
 * [Sync] If this file changes, update this header and any UI components that consume the appearance manifest shape.
 */

import {
  BaseDirectory,
  exists,
  mkdir,
  readDir,
  readFile,
  readTextFile,
  remove,
  writeFile,
  writeTextFile,
} from "@tauri-apps/plugin-fs";
export { readFile as readAppearanceFile };
import { appLocalDataDir, join } from "@tauri-apps/api/path";
import { convertFileSrc } from "@tauri-apps/api/core";
import { createAsyncCache } from "./client-cache.js";
import { extensionFromMime } from "./avatar-pipeline/image.js";
import { FAMILIES } from "./avatar-pipeline/families.js";
import {
  BUILTIN_TERRIER_APPEARANCE_ID,
  createBuiltinTerrierAppearance,
  defaultAudioCueSrcForFamily,
} from "./builtin-appearances.js";

const ROOT_DIR = "custom-appearances";
const MANIFEST_FILE = "manifest.json";
const VIDEO_DIR = "videos";
const BUILTIN_AUDIO_OVERRIDE_FILE = "audio-overrides.json";
const BUILTIN_AUDIO_OVERRIDE_DIR = `${ROOT_DIR}/${BUILTIN_TERRIER_APPEARANCE_ID}/audio-overrides`;
const APPEARANCE_LIST_CACHE_TTL_MS = 30_000;
const appearanceListCache = createAsyncCache({ ttlMs: APPEARANCE_LIST_CACHE_TTL_MS });

function uuid() {
  if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") {
    return crypto.randomUUID();
  }
  return `app-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

async function ensureRoot() {
  const has = await exists(ROOT_DIR, { baseDir: BaseDirectory.AppLocalData });
  if (!has) await mkdir(ROOT_DIR, { baseDir: BaseDirectory.AppLocalData, recursive: true });
}

async function ensureAppearanceDir(appearanceId) {
  await ensureRoot();
  const dir = `${ROOT_DIR}/${appearanceId}`;
  const has = await exists(dir, { baseDir: BaseDirectory.AppLocalData });
  if (!has) await mkdir(dir, { baseDir: BaseDirectory.AppLocalData, recursive: true });
  const videosDir = `${dir}/${VIDEO_DIR}`;
  const hasVideos = await exists(videosDir, { baseDir: BaseDirectory.AppLocalData });
  if (!hasVideos) await mkdir(videosDir, { baseDir: BaseDirectory.AppLocalData, recursive: true });
  return dir;
}

async function ensureBuiltinOverrideDir() {
  await ensureRoot();
  const has = await exists(BUILTIN_AUDIO_OVERRIDE_DIR, { baseDir: BaseDirectory.AppLocalData });
  if (!has) await mkdir(BUILTIN_AUDIO_OVERRIDE_DIR, { baseDir: BaseDirectory.AppLocalData, recursive: true });
  return BUILTIN_AUDIO_OVERRIDE_DIR;
}

async function absolutePathFor(relative) {
  const root = await appLocalDataDir();
  return join(root, relative);
}

let cachedRoot = null;
async function getRoot() {
  if (cachedRoot) return cachedRoot;
  cachedRoot = await appLocalDataDir();
  return cachedRoot;
}

async function readBuiltinAudioOverrides() {
  const rel = `${ROOT_DIR}/${BUILTIN_TERRIER_APPEARANCE_ID}/${BUILTIN_AUDIO_OVERRIDE_FILE}`;
  try {
    const raw = await readTextFile(rel, { baseDir: BaseDirectory.AppLocalData });
    const parsed = JSON.parse(raw);
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

async function writeBuiltinAudioOverrides(overrides) {
  await ensureRoot();
  const dir = `${ROOT_DIR}/${BUILTIN_TERRIER_APPEARANCE_ID}`;
  const has = await exists(dir, { baseDir: BaseDirectory.AppLocalData });
  if (!has) await mkdir(dir, { baseDir: BaseDirectory.AppLocalData, recursive: true });
  await writeTextFile(`${dir}/${BUILTIN_AUDIO_OVERRIDE_FILE}`, JSON.stringify(overrides, null, 2), {
    baseDir: BaseDirectory.AppLocalData,
  });
}

function appLocalAudioSrc(audioPath, root) {
  if (!audioPath || !root) return "";
  try {
    return convertFileSrc(joinPosix(root, audioPath));
  } catch {
    return "";
  }
}

async function getBuiltinTerrierAppearance() {
  const record = createBuiltinTerrierAppearance();
  const overrides = await readBuiltinAudioOverrides();
  let root = "";
  try {
    root = await getRoot();
  } catch {
    root = "";
  }
  record.families = record.families.map((familyRecord) => {
    const overridePath = overrides[familyRecord.family];
    if (!overridePath) return familyRecord;
    return {
      ...familyRecord,
      audioPath: overridePath,
      audioSrc: appLocalAudioSrc(overridePath, root),
      audioDefault: false,
    };
  });
  return record;
}

function withDefaultAudioCues(record) {
  return {
    ...record,
    families: (record.families || []).map((familyRecord) => {
      if (!familyRecord.ok || familyRecord.audioPath || familyRecord.audioSrc) return familyRecord;
      const audioSrc = defaultAudioCueSrcForFamily(familyRecord.family);
      if (!audioSrc) return familyRecord;
      return {
        ...familyRecord,
        audioSrc,
        audioDefault: true,
      };
    }),
  };
}

function buildUploadedVideoFamilies(selectedFamily, videoRel, originalFilename = "") {
  return FAMILIES.map((definition) => {
    const prompt = `用户上传的 ${definition.family} 状态视频`;
    if (definition.family === selectedFamily) {
      return {
        family: definition.family,
        ok: definition.family === selectedFamily,
        prompt,
        videoPath: videoRel,
        taskId: `local-upload-${Date.now().toString(36)}`,
        videoUrl: originalFilename || "",
      };
    }
    return {
      family: definition.family,
      ok: definition.family === selectedFamily,
      prompt,
      error: "尚未上传状态视频",
    };
  });
}

/**
 * Persist a freshly generated appearance.
 *
 * @param {{
 *   appearanceName?: string,
 *   personality?: string,
 *   provider: string,
 *   model?: string,
 *   baseUrl?: string,
 *   thinkingModel?: string,
 *   persona: object,
 *   imagePayload: { bytes: Uint8Array, mime: string, filename: string },
 *   families: Array<{ family: string, prompt: string, ok: boolean, videoBytes?: Uint8Array, error?: string, taskId?: string, videoUrl?: string }>,
 * }} input
 * @returns {Promise<AppearanceRecord>}
 */
export async function saveAppearance(input) {
  const id = uuid();
  const dir = await ensureAppearanceDir(id);

  const ext = extensionFromMime(input.imagePayload.mime);
  const sourceRel = `${dir}/source${ext}`;
  await writeFile(sourceRel, input.imagePayload.bytes, { baseDir: BaseDirectory.AppLocalData });

  const familiesOut = [];
  for (const f of input.families) {
    if (f.ok && f.videoBytes) {
      const videoRel = `${dir}/${VIDEO_DIR}/${f.family}.mp4`;
      await writeFile(videoRel, f.videoBytes, { baseDir: BaseDirectory.AppLocalData });
      familiesOut.push({
        family: f.family,
        ok: true,
        prompt: f.prompt,
        videoPath: videoRel,
        taskId: f.taskId,
        videoUrl: f.videoUrl,
      });
    } else {
      familiesOut.push({
        family: f.family,
        ok: false,
        prompt: f.prompt,
        error: f.error,
      });
    }
  }

  const manifest = {
    schema_version: 1,
    id,
    type: "custom",
    name: input.appearanceName?.trim() || "未命名形象",
    description: input.personality?.trim() || "",
    provider: input.provider,
    model: input.model || "",
    base_url: input.baseUrl || "",
    thinking_model: input.thinkingModel || "",
    persona: input.persona || {},
    source_image: sourceRel,
    source_mime: input.imagePayload.mime,
    families: familiesOut,
    created_at: new Date().toISOString(),
  };

  await writeTextFile(`${dir}/${MANIFEST_FILE}`, JSON.stringify(manifest, null, 2), {
    baseDir: BaseDirectory.AppLocalData,
  });

  invalidateAppearanceCache();
  return toRecord(manifest, await absolutePathFor(dir), await getRoot());
}

/**
 * Persist a user-uploaded MP4 as a custom appearance. The selected family gets
 * the uploaded video immediately; the other known state slots stay present so
 * the detail page can add or replace them one by one.
 *
 * @param {{
 *   appearanceName?: string,
 *   description?: string,
 *   family?: string,
 *   videoBytes: Uint8Array,
 *   originalFilename?: string,
 * }} input
 * @returns {Promise<AppearanceRecord>}
 */
export async function saveUploadedVideoAppearance(input) {
  if (!input?.videoBytes) throw new Error("请先选择一个 MP4 状态视频。");
  const selectedFamily = input.family || FAMILIES[0]?.family || "working";
  const id = uuid();
  const dir = await ensureAppearanceDir(id);
  const videoRel = `${dir}/${VIDEO_DIR}/${selectedFamily}.mp4`;
  await writeFile(videoRel, input.videoBytes, { baseDir: BaseDirectory.AppLocalData });

  const manifest = {
    schema_version: 1,
    id,
    type: "custom",
    name: input.appearanceName?.trim() || input.originalFilename?.replace(/\.mp4$/i, "") || "上传视频形象",
    description: input.description?.trim() || "自定义上传视频形象",
    provider: "local-upload",
    model: "uploaded-mp4",
    base_url: "",
    thinking_model: "",
    persona: {},
    source_image: "",
    source_mime: "",
    families: buildUploadedVideoFamilies(selectedFamily, videoRel, input.originalFilename),
    created_at: new Date().toISOString(),
  };

  await writeTextFile(`${dir}/${MANIFEST_FILE}`, JSON.stringify(manifest, null, 2), {
    baseDir: BaseDirectory.AppLocalData,
  });

  invalidateAppearanceCache();
  return toRecord(manifest, await absolutePathFor(dir), await getRoot());
}

/**
 * Read all stored appearances from disk.
 *
 * @returns {Promise<AppearanceRecord[]>}
 */
async function readAppearanceRecords() {
  const results = [];
  try {
    await ensureRoot();
    const entries = await readDir(ROOT_DIR, { baseDir: BaseDirectory.AppLocalData });
    const root = await getRoot();
    for (const entry of entries) {
      if (!entry.isDirectory) continue;
      if (entry.name === "builtin-terrier") continue;
      const rel = `${ROOT_DIR}/${entry.name}/${MANIFEST_FILE}`;
      try {
        const text = await readTextFile(rel, { baseDir: BaseDirectory.AppLocalData });
        const manifest = JSON.parse(text);
        const absDir = await absolutePathFor(`${ROOT_DIR}/${entry.name}`);
        results.push(toRecord(manifest, absDir, root));
      } catch (err) {
        // ignore broken manifest dirs
        console.warn("Failed to load appearance manifest:", rel, err);
      }
    }
  } catch (err) {
    console.warn("Failed to read app-local appearances; using built-ins only:", err);
    return [await getBuiltinTerrierAppearance()];
  }
  results.sort((a, b) => (a.created_at < b.created_at ? 1 : -1));
  return [await getBuiltinTerrierAppearance(), ...results];
}

/**
 * Read all stored appearances through a short-lived client cache.
 *
 * @param {{ force?: boolean }} options
 * @returns {Promise<AppearanceRecord[]>}
 */
export async function listAppearances({ force = false } = {}) {
  return appearanceListCache.get(readAppearanceRecords, { force });
}

export function getCachedAppearances() {
  return appearanceListCache.peek();
}

export function invalidateAppearanceCache() {
  appearanceListCache.invalidate();
}

export async function getAppearance(id) {
  if (id === BUILTIN_TERRIER_APPEARANCE_ID) {
    return getBuiltinTerrierAppearance();
  }
  const dir = `${ROOT_DIR}/${id}`;
  const text = await readTextFile(`${dir}/${MANIFEST_FILE}`, { baseDir: BaseDirectory.AppLocalData });
  const manifest = JSON.parse(text);
  const absDir = await absolutePathFor(dir);
  return toRecord(manifest, absDir, await getRoot());
}

export async function deleteAppearance(id) {
  const dir = `${ROOT_DIR}/${id}`;
  await remove(dir, { baseDir: BaseDirectory.AppLocalData, recursive: true });
  invalidateAppearanceCache();
}

/**
 * Read an app-local media file back into a Blob URL so UI previews can render
 * even when convertFileSrc() is unavailable or a packaged asset URL fails.
 */
export async function readAppearanceFileAsBlobUrl(filePath, mime = "application/octet-stream") {
  const bytes = await readFile(filePath, { baseDir: BaseDirectory.AppLocalData });
  const blob = new Blob([bytes], { type: mime });
  return URL.createObjectURL(blob);
}

/**
 * Read a video file back into a Blob URL so a <video> element can play it.
 * Useful when convertFileSrc() falls back to about:blank (e.g. dev:web mode).
 */
export async function readVideoAsBlobUrl(videoPath) {
  return readAppearanceFileAsBlobUrl(videoPath, "video/mp4");
}

export async function readAudioAsBlobUrl(audioPath) {
  return readAppearanceFileAsBlobUrl(audioPath, "audio/wav");
}

/**
 * Read the source image bytes back into memory (used by retry flow).
 */
export async function readAppearanceSourceImage(appearanceId) {
  const manifest = JSON.parse(
    await readTextFile(`${ROOT_DIR}/${appearanceId}/${MANIFEST_FILE}`, {
      baseDir: BaseDirectory.AppLocalData,
    }),
  );
  const bytes = await readFile(manifest.source_image, { baseDir: BaseDirectory.AppLocalData });
  return { bytes, mime: manifest.source_mime || "image/png" };
}

/**
 * Replace a single family's video (e.g. after a retry).
 */
export async function replaceFamilyVideo({ appearanceId, family, videoBytes, taskId, videoUrl, prompt }) {
  const dir = `${ROOT_DIR}/${appearanceId}`;
  const videoRel = `${dir}/${VIDEO_DIR}/${family}.mp4`;
  await writeFile(videoRel, videoBytes, { baseDir: BaseDirectory.AppLocalData });

  const manifest = JSON.parse(
    await readTextFile(`${dir}/${MANIFEST_FILE}`, { baseDir: BaseDirectory.AppLocalData }),
  );
  const idx = manifest.families.findIndex((f) => f.family === family);
  const next = {
    family,
    ok: true,
    prompt: prompt || manifest.families[idx]?.prompt || "",
    videoPath: videoRel,
    taskId,
    videoUrl,
  };
  if (manifest.families[idx]?.audioPath) next.audioPath = manifest.families[idx].audioPath;
  if (idx >= 0) manifest.families[idx] = next;
  else manifest.families.push(next);
  await writeTextFile(`${dir}/${MANIFEST_FILE}`, JSON.stringify(manifest, null, 2), {
    baseDir: BaseDirectory.AppLocalData,
  });
  invalidateAppearanceCache();
  return toRecord(manifest, await absolutePathFor(dir), await getRoot());
}

export async function replaceFamilyAudioCue({ appearanceId, family, audioBytes }) {
  if (appearanceId === BUILTIN_TERRIER_APPEARANCE_ID) {
    const dir = await ensureBuiltinOverrideDir();
    const audioRel = `${dir}/${family}.wav`;
    await writeFile(audioRel, audioBytes, { baseDir: BaseDirectory.AppLocalData });
    const overrides = await readBuiltinAudioOverrides();
    overrides[family] = audioRel;
    await writeBuiltinAudioOverrides(overrides);
    invalidateAppearanceCache();
    return getBuiltinTerrierAppearance();
  }

  const dir = await ensureAppearanceDir(appearanceId);
  const audioRel = `${dir}/${VIDEO_DIR}/${family}.wav`;
  await writeFile(audioRel, audioBytes, { baseDir: BaseDirectory.AppLocalData });

  const manifest = JSON.parse(
    await readTextFile(`${dir}/${MANIFEST_FILE}`, { baseDir: BaseDirectory.AppLocalData }),
  );
  const idx = manifest.families.findIndex((f) => f.family === family);
  if (idx < 0) throw new Error(`未找到状态素材: ${family}`);
  manifest.families[idx] = { ...manifest.families[idx], audioPath: audioRel };
  await writeTextFile(`${dir}/${MANIFEST_FILE}`, JSON.stringify(manifest, null, 2), {
    baseDir: BaseDirectory.AppLocalData,
  });
  invalidateAppearanceCache();
  return toRecord(manifest, await absolutePathFor(dir), await getRoot());
}

export async function removeFamilyAudioCue({ appearanceId, family }) {
  if (appearanceId === BUILTIN_TERRIER_APPEARANCE_ID) {
    const overrides = await readBuiltinAudioOverrides();
    if (overrides[family]) {
      await remove(overrides[family], { baseDir: BaseDirectory.AppLocalData }).catch(() => {});
      delete overrides[family];
      await writeBuiltinAudioOverrides(overrides);
    }
    invalidateAppearanceCache();
    return getBuiltinTerrierAppearance();
  }

  const dir = `${ROOT_DIR}/${appearanceId}`;
  const manifest = JSON.parse(
    await readTextFile(`${dir}/${MANIFEST_FILE}`, { baseDir: BaseDirectory.AppLocalData }),
  );
  const idx = manifest.families.findIndex((f) => f.family === family);
  if (idx < 0) throw new Error(`未找到状态素材: ${family}`);
  if (manifest.families[idx].audioPath) {
    await remove(manifest.families[idx].audioPath, { baseDir: BaseDirectory.AppLocalData }).catch(() => {});
  }
  delete manifest.families[idx].audioPath;
  delete manifest.families[idx].audioSrc;
  await writeTextFile(`${dir}/${MANIFEST_FILE}`, JSON.stringify(manifest, null, 2), {
    baseDir: BaseDirectory.AppLocalData,
  });
  invalidateAppearanceCache();
  return toRecord(manifest, await absolutePathFor(dir), await getRoot());
}

/**
 * @typedef {{
 *   id: string,
 *   type: 'custom' | 'codex-import' | 'builtin',
 *   name: string,
 *   description: string,
 *   provider: string,
 *   model: string,
 *   thinking_model: string,
 *   persona: object,
 *   source_image?: string,
 *   source_image_src?: string,
 *   source_mime?: string,
 *   source_preview?: string,
 *   source_preview_src?: string,
 *   source_preview_mime?: string,
 *   families: Array<{ family: string, ok: boolean, prompt: string, videoPath?: string, videoSrc?: string, audioPath?: string, audioSrc?: string, error?: string, taskId?: string, videoUrl?: string }>,
 *   created_at: string,
 *   absolute_dir: string,
 * }} AppearanceRecord
 */

function joinPosix(a, b) {
  if (!a) return b;
  if (!b) return a;
  const left = a.replace(/[\\/]+$/, "");
  const right = b.replace(/^[\\/]+/, "");
  return `${left}/${right}`;
}

function toRecord(manifest, absDir, root) {
  const record = {
    ...manifest,
    absolute_dir: absDir,
  };
  // Pre-compute convertFileSrc URLs (only valid in Tauri runtime; in web preview these will be empty
  // strings and the UI will fall back to readVideoAsBlobUrl).
  try {
    if (manifest.source_image && root) {
      record.source_image_src = convertFileSrc(joinPosix(root, manifest.source_image));
    }
    if (manifest.source_preview && root) {
      record.source_preview_src = convertFileSrc(joinPosix(root, manifest.source_preview));
    }
    record.families = manifest.families.map((f) => {
      const next = { ...f };
      if (f.videoPath && root) {
        next.videoSrc = convertFileSrc(joinPosix(root, f.videoPath));
      }
      if (f.audioPath && root) {
        next.audioSrc = convertFileSrc(joinPosix(root, f.audioPath));
      }
      return next;
    });
  } catch {
    // convertFileSrc is unavailable in plain web context; UI will use readVideoAsBlobUrl instead.
  }
  return withDefaultAudioCues(record);
}
