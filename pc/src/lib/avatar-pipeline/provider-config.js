/**
 * [Input] Provider id, optional browser storage, and an optional compile-time internal Volcengine API key fallback.
 * [Output] Shared avatar video provider list, independently resolved Ark image-edit config, user-config-first persistence helpers, and normalized generation config.
 * [Pos] config node in pc/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header and `pc/src/.folder.md`.
 */

import { DEFAULT_THINKING_MODEL } from "./thinking-model.js";
import {
  DEFAULT_VOLCANO_BASE_URL,
  DEFAULT_VOLCANO_VIDEO_MODEL,
} from "./providers/volcano.js";
import { DEFAULT_VOLCANO_IMAGE_MODEL } from "./providers/volcano-image.js";

export const STORAGE_KEY_PREFIX = "claw-pet.video-gen-config.";
export const DEFAULT_PROVIDER_ID = "volcengine";
export const VOLCENGINE_BASE_URL = DEFAULT_VOLCANO_BASE_URL;
export const VOLCENGINE_THINKING_MODEL = DEFAULT_THINKING_MODEL;
export const VOLCENGINE_IMAGE_MODEL = DEFAULT_VOLCANO_IMAGE_MODEL;
export const VOLCENGINE_CUSTOM_MODEL_OPTION = "__custom__";
export const VOLCENGINE_VIDEO_MODEL_SUGGESTIONS = [
  "doubao-seedance-1-5-pro-251215",
  DEFAULT_VOLCANO_VIDEO_MODEL,
];

const INTERNAL_VOLCENGINE_API_KEY = (
  typeof __PET_MANAGER_INTERNAL_CONTENT_API_KEY__ === "string"
    ? __PET_MANAGER_INTERNAL_CONTENT_API_KEY__
    : ""
).trim();

export const VIDEO_PROVIDERS = [
  {
    id: "volcengine",
    label: "火山引擎",
    sub: "Ark / Seedance / 即梦",
    baseUrl: VOLCENGINE_BASE_URL,
    models: VOLCENGINE_VIDEO_MODEL_SUGGESTIONS,
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
  return typeof localStorage === "undefined" ? null : localStorage;
}

export function providerById(providerId = DEFAULT_PROVIDER_ID) {
  return VIDEO_PROVIDERS.find((item) => item.id === providerId) || VIDEO_PROVIDERS[0];
}

export function resolveProviderApiKey(
  providerId,
  savedApiKey,
  internalApiKey = INTERNAL_VOLCENGINE_API_KEY,
) {
  if (typeof savedApiKey === "string" && savedApiKey.trim()) return savedApiKey;
  return providerId === "volcengine" ? String(internalApiKey || "").trim() : "";
}

function parseStoredProviderConfig(providerId, storage = defaultStorage()) {
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
    thinkingModel: isVolcengine
      ? VOLCENGINE_THINKING_MODEL
      : typeof saved.thinkingModel === "string"
        ? saved.thinkingModel
        : provider.thinkingModel || "",
    imageModel: isVolcengine
      ? VOLCENGINE_IMAGE_MODEL
      : typeof saved.imageModel === "string"
        ? saved.imageModel
        : "",
    fastGeneration: saved.fastGeneration !== false,
    advanced: { ...DEFAULT_ADVANCED, ...(saved.advanced || {}) },
  };
}

export function loadArkImageConfig(storage = defaultStorage()) {
  const config = loadProviderConfig("volcengine", storage);
  return {
    apiKey: config.apiKey,
    baseUrl: VOLCENGINE_BASE_URL,
    model: VOLCENGINE_IMAGE_MODEL,
  };
}

export function saveProviderConfig(providerId, config, storage = defaultStorage()) {
  if (!storage) return;
  try {
    storage.setItem(`${STORAGE_KEY_PREFIX}${providerId}`, JSON.stringify(config));
  } catch {
    /* ignore quota */
  }
}
