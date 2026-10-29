/**
 * [Input] Provider id, optional browser storage, and user-provided API credentials.
 * [Output] Shared video provider config and user-config-first persistence; failed writes throw instead of reporting a false save. Ark models are discovered live.
 * [Pos] config node in pc/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header and `pc/src/.folder.md`.
 */

import { DEFAULT_THINKING_MODEL } from "./thinking-model.js";
import { invoke } from "@tauri-apps/api/core";
import {
  DEFAULT_VOLCANO_BASE_URL,
} from "./providers/volcano.js";
import { DEFAULT_VOLCANO_IMAGE_MODEL } from "./providers/volcano-image.js";

export const STORAGE_KEY_PREFIX = "claw-pet.video-gen-config.";
export const DEFAULT_PROVIDER_ID = "volcengine";
export const VOLCENGINE_BASE_URL = DEFAULT_VOLCANO_BASE_URL;
export const VOLCENGINE_THINKING_MODEL = DEFAULT_THINKING_MODEL;
export const VOLCENGINE_IMAGE_MODEL = DEFAULT_VOLCANO_IMAGE_MODEL;
let nativeConfigs = {};
let initialization = null;
let nativeReadError = "";
const nativeRuntime = () => typeof window !== "undefined" && Boolean(window.__TAURI_INTERNALS__);

export function initializeProviderConfigs(storage = defaultStorage(), call = invoke) {
  if (!nativeRuntime()) return Promise.resolve();
  if (!initialization) initialization = migrateProviderConfigs(storage, call)
    .then(() => { nativeReadError = ""; })
    .catch(error => { nativeReadError = error?.message || String(error); throw error; })
    .finally(() => { initialization = null; });
  return initialization;
}
async function migrateProviderConfigs(storage, call) {
  const saved = await call("load_generation_provider_configs");
  if (!saved || typeof saved !== "object" || Array.isArray(saved)) throw Error("安全配置读取失败");
  for (const provider of VIDEO_PROVIDERS) {
    let legacy;
    try {
      const raw = storage?.getItem(`${STORAGE_KEY_PREFIX}${provider.id}`);
      if (raw == null) continue;
      legacy = JSON.parse(raw);
      if (!legacy || typeof legacy !== "object" || Array.isArray(legacy)) throw Error();
    } catch {
      throw Error("旧版生成配置无法读取，原数据已保留；请先恢复本地存储再重试");
    }
    if (!legacy) continue;
    // Native data wins on retry. Remove browser plaintext only after native commit succeeded.
    if (!saved[provider.id]) {
      await call("save_generation_provider_config", { providerId: provider.id, config: legacy });
      saved[provider.id] = legacy;
    }
    try { storage.removeItem(`${STORAGE_KEY_PREFIX}${provider.id}`); }
    catch { throw Error("安全配置已保存，但旧版本地配置尚未清理，请重试"); }
  }
  nativeConfigs = saved;
}


export const VIDEO_PROVIDERS = [
  {
    id: "volcengine",
    label: "火山引擎",
    sub: "Ark / Seedance / 即梦",
    baseUrl: VOLCENGINE_BASE_URL,
    models: [],
    thinkingModel: VOLCENGINE_THINKING_MODEL,
    imageModel: VOLCENGINE_IMAGE_MODEL,
  },
  {
    id: "kling",
    label: "可灵 AI",
    sub: "Kling 视频生成",
    baseUrl: "https://api-beijing.klingai.com",
    models: ["kling-v2-master", "kling-v1-6", "kling-v1-5"],
    thinkingModel: "",
  },
  {
    id: "custom",
    label: "其他兼容 API",
    sub: "聚合 / 代理 / OpenAI 风格",
    baseUrl: "https://api.example.com",
    models: [],
    thinkingModel: "",
  },
];

export const DEFAULT_ADVANCED = {
  authHeader: "Authorization",
  authPrefix: "Bearer",
  createPath: "/v1/video/generations",
  queryPath: "/v1/tasks/{id}",
  webhookUrl: "",
  timeoutMs: 120000,
  pollingIntervalMs: 3000,
  resultPath: "data[0].url",
};

function defaultStorage() {
  try { return typeof localStorage === "undefined" ? null : localStorage; }
  catch { return null; }
}

export function pickVideoParameters(value) {
  const result = {};
  for (const key of ["duration", "resolution", "seed", "cameraFixed", "generateAudio", "watermark"]) {
    if (value?.[key] != null) result[key] = value[key];
  }
  return result;
}

export function providerById(providerId = DEFAULT_PROVIDER_ID) {
  return VIDEO_PROVIDERS.find((item) => item.id === providerId) || VIDEO_PROVIDERS[0];
}

export function resolveProviderApiKey(
  providerId,
  savedApiKey,
) {
  if (typeof savedApiKey === "string" && savedApiKey.trim()) return savedApiKey;
  return "";
}

function parseStoredProviderConfig(providerId, storage = defaultStorage(), native = true) {
  if (native && nativeRuntime()) return nativeConfigs[providerId] || null;
  if (!storage) return null;
  try {
    const raw = storage.getItem(`${STORAGE_KEY_PREFIX}${providerId}`);
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    return parsed && typeof parsed === "object" ? parsed : null;
  } catch {
    return null;
  }
}

export function loadProviderConfig(providerId = DEFAULT_PROVIDER_ID, storage = defaultStorage()) {
  const provider = providerById(providerId);
  const saved = parseStoredProviderConfig(provider.id, storage) || {};
  const isVolcengine = provider.id === "volcengine";
  const hasSavedFastGeneration = typeof saved.fastGeneration === "boolean";
  const savedModel = typeof saved.model === "string" ? saved.model : "";

  return {
    providerId: provider.id,
    storageError: nativeRuntime() ? nativeReadError : "",
    provider,
    apiKey: resolveProviderApiKey(provider.id, saved.apiKey),
    accessKey: typeof saved.accessKey === "string" ? saved.accessKey : "",
    secretKey: typeof saved.secretKey === "string" ? saved.secretKey : "",
    baseUrl: isVolcengine
      ? VOLCENGINE_BASE_URL
      : typeof saved.baseUrl === "string"
        ? saved.baseUrl
        : provider.baseUrl,
    model: savedModel && hasSavedFastGeneration ? savedModel : provider.models[0] || savedModel || "",
    thinkingModel: isVolcengine && (!saved.thinkingModel || saved.thinkingModel === "doubao-seed-2-0-pro-260215")
      ? VOLCENGINE_THINKING_MODEL
      : typeof saved.thinkingModel === "string"
        ? saved.thinkingModel
        : provider.thinkingModel || "",
    imageModel: typeof saved.imageModel === "string" && saved.imageModel.trim()
      ? saved.imageModel.trim()
      : provider.imageModel || "",
    fastGeneration: saved.fastGeneration !== false,
    videoParameters: pickVideoParameters(saved.videoParameters),
    advanced: { ...DEFAULT_ADVANCED, ...(saved.advanced || {}) },
  };
}

export function loadArkImageConfig(storage = defaultStorage()) {
  const config = loadProviderConfig("volcengine", storage);
  return {
    apiKey: config.apiKey,
    baseUrl: VOLCENGINE_BASE_URL,
    model: config.imageModel,
  };
}

export function saveProviderConfig(providerId, config, storage = defaultStorage()) {
  if (nativeRuntime()) {
    if (nativeReadError) return Promise.reject(new Error(`原配置尚未读取，不会覆盖：${nativeReadError}`));
    return invoke("save_generation_provider_config", { providerId, config }).then(() => {
      nativeConfigs = { ...nativeConfigs, [providerId]: structuredClone(config) };
    });
  }
  if (!storage) throw new Error("本地存储不可用，配置未保存");
  try {
    storage.setItem(`${STORAGE_KEY_PREFIX}${providerId}`, JSON.stringify(config));
  } catch {
    throw new Error("本地存储写入失败，配置未保存；请检查可用磁盘空间后重试");
  }
}
