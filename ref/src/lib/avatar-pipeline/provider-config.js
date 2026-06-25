/**
 * [Input] Provider id plus optional browser storage.
 * [Output] Shared avatar video provider list, localStorage persistence helpers, and normalized generation config.
 * [Pos] config node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import { DEFAULT_THINKING_MODEL } from "./thinking-model.js";
import {
  DEFAULT_VOLCANO_BASE_URL,
  DEFAULT_VOLCANO_VIDEO_MODEL,
} from "./providers/volcano.js";

export const STORAGE_KEY_PREFIX = "claw-pet.video-gen-config.";
export const DEFAULT_PROVIDER_ID = "volcengine";
export const VOLCENGINE_BASE_URL = DEFAULT_VOLCANO_BASE_URL;
export const VOLCENGINE_THINKING_MODEL = DEFAULT_THINKING_MODEL;
export const VOLCENGINE_CUSTOM_MODEL_OPTION = "__custom__";
export const VOLCENGINE_VIDEO_MODEL_SUGGESTIONS = [
  "doubao-seedance-1-5-pro-251215",
  DEFAULT_VOLCANO_VIDEO_MODEL,
];

export const VIDEO_PROVIDERS = [
  {
    id: "volcengine",
    label: "火山引擎",
    sub: "Ark / Seedance / 即梦",
    baseUrl: VOLCENGINE_BASE_URL,
    models: VOLCENGINE_VIDEO_MODEL_SUGGESTIONS,
    thinkingModel: VOLCENGINE_THINKING_MODEL,
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
    apiKey: typeof saved.apiKey === "string" ? saved.apiKey : "",
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
    fastGeneration: saved.fastGeneration !== false,
    advanced: { ...DEFAULT_ADVANCED, ...(saved.advanced || {}) },
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
