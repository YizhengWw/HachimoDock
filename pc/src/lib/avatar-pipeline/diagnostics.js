/** Bounded, allowlisted local diagnostics. No URLs, credentials, prompts or image data. */
const STORAGE_SLOT = "pet-generation-diagnostics-v1";
const STAGES = { image_edit: "图片背景处理", thinking: "分析图片并生成提示词", reference_review: "参考图自动检查", trial_review: "试片自动检查", video_submit: "提交视频任务", video_poll: "查询视频进度", download: "下载生成素材", processing: "处理参考图片", generating: "生成视频", saving: "保存形象", validating: "检查模型配置" };
const PHASES = { prepare: "准备请求", connect: "建立连接", request_wait: "发送请求/等待响应", response_body: "读取响应内容", complete: "响应完成", parse: "解析检查结论", stage: "生成阶段", unknown: "未取得详细阶段" };
const OUTCOMES = new Set(["pending", "success", "http_error", "timeout", "transport_error", "configuration_error", "invalid_output", "failed", "stage"]);
let memory = [];
export function safeRequestId(value) {
  return typeof value === "string" && /^[a-zA-Z0-9_.:-]{1,128}$/.test(value) && !value.startsWith("sk-") ? value : null;
}
export function sanitizeDiagnostic(value = {}) {
  value = value && typeof value === "object" ? value : {};
  const number = (n) => Number.isFinite(n) && n >= 0 ? Math.min(Math.round(n), Number.MAX_SAFE_INTEGER) : 0;
  return {
    localId: safeRequestId(value.localId), timestampMs: number(value.timestampMs),
    stage: Object.hasOwn(STAGES, value.stage) ? value.stage : "other",
    phase: Object.hasOwn(PHASES, value.phase) ? value.phase : "unknown",
    elapsedMs: number(value.elapsedMs), timeoutMs: number(value.timeoutMs),
    status: Number.isInteger(value.status) && value.status >= 100 && value.status <= 599 ? value.status : null,
    requestId: safeRequestId(value.requestId), outcome: OUTCOMES.has(value.outcome) ? value.outcome : "failed",
  };
}
export function getGenerationDiagnostics(since = 0) {
  try {
    const raw = globalThis.localStorage?.getItem(STORAGE_SLOT);
    if (raw && raw.length <= 128 * 1024) {
      const parsed = JSON.parse(raw);
      if (Array.isArray(parsed)) memory = parsed.slice(-100).map(sanitizeDiagnostic);
    }
  } catch { /* Diagnostics must never prevent generation. */ }
  return memory.filter((row) => row.timestampMs >= since).map(sanitizeDiagnostic);
}
export function recordDiagnostic(value) {
  const row = sanitizeDiagnostic(value);
  if (row.stage === "other") return row;
  const rows = getGenerationDiagnostics().filter((item) => !row.localId || item.localId !== row.localId);
  memory = [...rows, row].slice(-100);
  try { globalThis.localStorage?.setItem(STORAGE_SLOT, JSON.stringify(memory)); } catch { /* quota/disabled storage */ }
  return row;
}
export function formatDiagnostic(value) {
  const d = sanitizeDiagnostic(value);
  return `${STAGES[d.stage] || "云服务请求"} · ${PHASES[d.phase]} · ${(d.elapsedMs / 1000).toFixed(1)} 秒${d.timeoutMs ? ` / 上限 ${d.timeoutMs / 1000} 秒` : ""}${d.status ? ` · HTTP ${d.status}` : ""} · 请求编号：${d.requestId || "未取得"}${d.localId ? ` · 本地追踪：${d.localId}` : ""}`;
}
export function parseNativeDiagnostic(error) {
  const raw = typeof error === "string" ? error : error?.message;
  if (!raw?.startsWith("PET_HTTP_DIAGNOSTIC:")) return null;
  try { return sanitizeDiagnostic(JSON.parse(raw.slice("PET_HTTP_DIAGNOSTIC:".length)).diagnostic); } catch { return null; }
}
export function transportHint(d) {
  if (d.outcome === "timeout") {
    if (d.phase === "connect") return "建立连接超时，尚未取得服务响应。";
    if (d.phase === "response_body") return "已收到服务响应，但读取内容超时。";
    return "发送请求或等待响应超时；服务端可能仍在处理，请勿重复提交。";
  }
  return d.outcome === "configuration_error" ? "网络配置或请求准备失败，请检查云服务网络设置。" : "传输失败，请检查云服务连接；不自动重复提交。";
}
