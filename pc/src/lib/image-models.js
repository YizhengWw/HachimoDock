import { pipelineFetch, readJsonOrThrow } from "./avatar-pipeline/http.js";
import { modelsUrl } from "./text-models.js";
import { DEFAULT_VOLCANO_IMAGE_MODEL, VOLCANO_IMAGE_MODEL_FALLBACKS } from "./avatar-pipeline/providers/volcano-image.js";

export const IMAGE_MODEL_PRESETS = [DEFAULT_VOLCANO_IMAGE_MODEL, ...VOLCANO_IMAGE_MODEL_FALLBACKS];

export function isImageModelShutdown(row) {
  const status = row?.lifecycle?.status || row?.lifecycle || row?.status;
  return typeof status === "string" && /^(shutdown|offline|retired|eos|已下线)$/i.test(status.trim());
}

export function imageModelOptions({ models, shutdownModels }, current = "") {
  const shutdown = new Set(shutdownModels);
  return [...new Set([...IMAGE_MODEL_PRESETS, ...models, ...(current ? [current] : [])])]
    .filter(id => !shutdown.has(id));
}

export function normalizeImageModels(rows) {
  const ids = new Set();
  for (const row of rows || []) {
    const id = typeof row === "string" ? row.trim() : typeof row?.id === "string" ? row.id.trim() : "";
    if (!id || id.length > 200 || ids.has(id)) continue;
    if (row?.available === false || row?.enabled === false || row?.active === false) continue;
    const status = row?.lifecycle?.status || row?.lifecycle || row?.status;
    if (isImageModelShutdown(row) || (typeof status === "string" && /disabled|unavailable/i.test(status))) continue;
    const input = row?.modalities?.input_modalities;
    const output = row?.modalities?.output_modalities;
    if ((Array.isArray(input) && !input.includes("image")) || (Array.isArray(output) && !output.includes("image"))) continue;
    if ((!Array.isArray(input) || !Array.isArray(output)) && !/seedream/i.test(id)) continue;
    ids.add(id);
  }
  return [...ids].sort((a, b) => a.localeCompare(b, "en", { numeric: true }));
}

export async function listImageModelCatalog({ apiKey, baseUrl, signal }, fetcher = pipelineFetch) {
  if (!apiKey?.trim()) throw Error("请先填写火山引擎 API Key");
  const response = await fetcher(modelsUrl(baseUrl), {
    method: "GET", signal, timeoutMs: 15000, headers: { Authorization: `Bearer ${apiKey.trim()}` },
  });
  const value = await readJsonOrThrow(response, "图片模型列表");
  if (signal?.aborted) throw new DOMException("aborted", "AbortError");
  if (!Array.isArray(value?.data) || value.has_more) throw Error("模型目录不完整，可手动填写模型或接入点 ID");
  return {
    models: normalizeImageModels(value.data),
    shutdownModels: value.data.filter(isImageModelShutdown)
      .map(row => typeof row?.id === "string" ? row.id.trim() : "").filter(Boolean),
  };
}

export async function listImageModels(config, fetcher = pipelineFetch) {
  return (await listImageModelCatalog(config, fetcher)).models;
}
