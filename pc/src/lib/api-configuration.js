/**
 * [Input] Browser API-configuration changes plus provider/ASR status values.
 * [Output] Shared API configuration event, fixed ASR/TTS 2.0 defaults, and credential-readiness helpers used by settings and feature pages.
 * [Pos] config helper in pc/src/lib
 * [Sync] If this file changes, update this header and `pc/src/.folder.md`.
 */

export const API_CONFIGURATION_UPDATED_EVENT = "pet-manager:api-configuration-updated";

/** 对话大模型预设（与 Rust `voice_chat_settings::LLM_PRESETS` 一致）：用户只填 Key。 */
export const LLM_PRESETS = [
  { id: "deepseek", label: "DeepSeek", baseUrl: "https://api.deepseek.com", model: "deepseek-flash" },
  { id: "doubao", label: "豆包", baseUrl: "https://ark.cn-beijing.volces.com/api/v3", model: "doubao-seed-2-0-lite-260428" },
  { id: "mimo", label: "MiMo", baseUrl: "https://api.xiaomimimo.com/v1", model: "mimo-v2.5" },
];

export const DEFAULT_TTS_RESOURCE_ID = "seed-tts-2.0";

export function describeLlmPreset(providerId) {
  const preset = LLM_PRESETS.find((item) => item.id === providerId);
  if (!preset) return "自定义 OpenAI 兼容端点";
  return `${preset.label} · ${preset.model} · ${preset.baseUrl}`;
}

export const ASR_RESOURCE_OPTIONS = [
  { id: "volc.seedasr.sauc.duration", label: "豆包 ASR 2.0" },
];

export function providerCredentialsConfigured(providerId, config = {}) {
  if (providerId === "kling") {
    return Boolean(config.accessKey?.trim() && config.secretKey?.trim());
  }
  return Boolean(config.apiKey?.trim());
}
export function emitApiConfigurationUpdated(detail = {}) {
  if (typeof window === "undefined") return;
  window.dispatchEvent(new CustomEvent(API_CONFIGURATION_UPDATED_EVENT, { detail }));
}
