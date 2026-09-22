/**
 * [Input] Browser API-configuration changes plus provider/ASR status values.
 * [Output] Shared API configuration event, editable LLM defaults, provider/endpoint-specific saved key masks, fixed ASR/TTS 2.0 defaults, and readiness helpers.
 * [Pos] config helper in pc/src/lib
 * [Sync] If this file changes, update this header and `pc/src/.folder.md`.
 */

export const API_CONFIGURATION_UPDATED_EVENT = "pet-manager:api-configuration-updated";
export const DEFAULT_LLM_PROVIDER = "doubao";

/** 对话大模型默认值（与 Rust `voice_chat_settings::LLM_PRESETS` 一致），模型名可修改。 */
export const LLM_PRESETS = [
  { id: "deepseek", label: "DeepSeek", baseUrl: "https://api.deepseek.com", model: "deepseek-flash" },
  { id: "doubao", label: "豆包", baseUrl: "https://ark.cn-beijing.volces.com/api/v3", model: "doubao-seed-2-0-lite-260428" },
  { id: "mimo", label: "MiMo", baseUrl: "https://api.xiaomimimo.com/v1", model: "mimo-v2.6-flash" },
];

export const DEFAULT_TTS_RESOURCE_ID = "seed-tts-2.0";

function restoreLlmCredential(current) {
  const endpoint = (current.llmBaseUrl || "").trim().replace(/\/+$/, "") || "https://api.deepseek.com";
  const scope = current.llmProvider === "custom" ? `custom:${endpoint}` : current.llmProvider;
  const masked = current.llmApiKeyMasks?.[scope] || "";
  return { ...current, llmApiKey: "", llmApiKeyMasked: masked, llmConfigured: Boolean(masked),
    llmTone: "muted", llmMessage: masked ? "已读取该服务的 API Key，点击保存应用切换。"
      : "此服务尚未配置 API Key，请填写后保存；不会沿用其他服务的 Key。" };
}

export function selectLlmProvider(current, providerId) {
  if (current.llmProvider === providerId) return current;
  const preset = LLM_PRESETS.find((item) => item.id === providerId);
  return restoreLlmCredential({ ...current, llmProvider: providerId,
    llmBaseUrl: preset?.baseUrl || current.llmCustomBaseUrl || "", llmModel: preset?.model || "" });
}

export function changeLlmEndpoint(current, baseUrl) {
  const normalize = (value) => (value || "").trim().replace(/\/+$/, "");
  const changed = current.llmProvider === "custom" && normalize(current.llmBaseUrl) !== normalize(baseUrl);
  const next = { ...current, llmBaseUrl: baseUrl };
  return changed ? restoreLlmCredential(next) : next;
}

export function describeLlmPreset(providerId, model) {
  const preset = LLM_PRESETS.find((item) => item.id === providerId);
  if (!preset) return "自定义 OpenAI 兼容端点";
  return `${preset.label} · ${model?.trim() || preset.model} · ${preset.baseUrl}`;
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
