/** Read-only model discovery. Catalog availability is not inference permission.
 * Keep explicit choices and endpoint IDs; never silently upgrade to a paid Pro. */
import { pipelineFetch, readJsonOrThrow } from "./avatar-pipeline/http.js";

export const DEFAULT_LITE_MODEL = "doubao-seed-2-1-lite-260915";
export const DOUBAO_PRO_MODEL = "doubao-seed-2-1-pro-260915";
export const DOUBAO_TEXT_MODELS = [DEFAULT_LITE_MODEL, DOUBAO_PRO_MODEL];

export function normalizeTextModels(rows, vision = false) {
  const ids = new Set();
  for (const row of rows || []) {
    const id = typeof row === "string" ? row.trim() : typeof row?.id === "string" ? row.id.trim() : "";
    if (!id || ids.has(id) || id.length > 200) continue;
    if (row?.available === false || row?.enabled === false || row?.active === false) continue;
    const status = row?.lifecycle?.status || row?.lifecycle || row?.status;
    if (typeof status === "string" && /retir|shutdown|disabled|unavailable/i.test(status)) continue;
    const input = row?.modalities?.input_modalities;
    const output = row?.modalities?.output_modalities;
    if ((Array.isArray(output) && !output.includes("text"))
      || (vision && Array.isArray(input) && !input.includes("image"))) continue;
    if (/seedance|seedream|embedding|rerank|tts|asr/i.test(id)) continue;
    ids.add(id);
  }
  return [...ids].sort((a, b) => a.localeCompare(b, "en", { numeric: true }));
}

export function latestLiteModel(models) {
  return models.filter(id => /^doubao-seed-\d+-\d+-lite-\d{6,8}$/.test(id))
    .sort((a, b) => b.localeCompare(a, "en", { numeric: true }))[0] || "";
}

export function modelsUrl(baseUrl) {
  const url = new URL(baseUrl);
  if (url.protocol !== "https:" || url.username || url.password || url.search || url.hash) throw Error("请填写不含凭据的 HTTPS 接口地址");
  const base = url.href.replace(/\/+$/, "");
  return /\/v\d+$/.test(base) ? `${base}/models` : `${base}/api/v3/models`;
}

export async function listThinkingModels({ apiKey, baseUrl, signal }, fetcher = pipelineFetch) {
  if (!apiKey?.trim()) throw Error("请先保存火山引擎 API Key");
  const response = await fetcher(modelsUrl(baseUrl), { method: "GET", signal, timeoutMs: 15000,
    headers: { Authorization: `Bearer ${apiKey.trim()}` } });
  const value = await readJsonOrThrow(response, "模型列表");
  if (signal?.aborted) throw new DOMException("aborted", "AbortError");
  if (!Array.isArray(value?.data) || value.has_more) throw Error("模型目录不完整，可手动填写模型或接入点 ID");
  return normalizeTextModels(value.data, true);
}
