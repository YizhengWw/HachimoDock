/** Advisory Ark catalog with version/date-ranked Fast defaults. Listing is not proof of inference permission; keep endpoint-ID entry available.
 * No management credentials, billable probes or persisted catalog credentials. */
import { pipelineFetch } from "./http.js";
import { DEFAULT_VOLCANO_BASE_URL, isVolcanoModelRejected } from "./providers/volcano.js";

function modelVersion(id) {
  const match = id.match(/seedance[-_.](\d+)[-_.](\d+)(?:[-_.](\d{1,2})(?=[-_.]|$))?/i);
  return match ? match.slice(1).map((part) => Number(part || 0)) : [0, 0, 0];
}

function modelDate(id) {
  const date = id.match(/(?:^|[-_.])(\d{8}|\d{6})(?=$|[-_.])/)?.[1] || "";
  return Number(date.length === 6 ? `20${date}` : date);
}

// Only initialize an empty automatic selection. Never overwrite a saved/manual
// model (including ep-* IDs), or silently substitute a non-Fast model.
export function selectDefaultVideoModel(models, currentModel = "", manual = false) {
  if (manual || currentModel.trim()) return currentModel;
  const candidates = models.filter(({ id }) => /seedance/i.test(id) && /(?:^|[-_.])fast(?:$|[-_.])/i.test(id));
  candidates.sort((a, b) => {
    const av = modelVersion(a.id);
    const bv = modelVersion(b.id);
    for (let i = 0; i < av.length; i += 1) {
      if (av[i] !== bv[i]) return bv[i] - av[i];
    }
    return modelDate(b.id) - modelDate(a.id) || b.id.localeCompare(a.id, "en", { numeric: true });
  });
  return candidates[0]?.id || "";
}

export async function listVideoModels({ apiKey, baseUrl = DEFAULT_VOLCANO_BASE_URL, signal }, fetcher = pipelineFetch) {
  if (!apiKey?.trim()) throw new Error("请先在 API 配置中填写火山 API Key。");
  const response = await fetcher(`${baseUrl.replace(/\/+$/, "")}/api/v3/models`, {
    method: "GET", signal, timeoutMs: 15000, headers: { Authorization: `Bearer ${apiKey.trim()}` },
  });
  if (signal?.aborted) throw new DOMException("aborted", "AbortError");
  if (!response.ok) {
    if ([401, 403].includes(response.status)) throw new Error("模型列表鉴权失败，请检查 API Key 权限。");
    throw new Error(`模型列表获取失败（HTTP ${response.status}），请刷新重试。`);
  }
  const json = await response.json();
  if (!Array.isArray(json?.data) || json.has_more) throw new Error("服务未返回完整的模型列表，请重试或手动填写模型。");
  return normalizeVideoModels(json.data).filter((item) => !isVolcanoModelRejected(apiKey, item.id));
}

export function normalizeVideoModels(rows) {
  const ids = new Set();
  return rows.filter((row) => {
    const id = typeof row?.id === "string" ? row.id.trim() : "";
    if (!id || ids.has(id) || !/seedance/i.test(id)) return false;
    if (row.available === false || row.enabled === false || row.active === false) return false;
    const status = row.lifecycle?.status || row.lifecycle || row.status;
    if (typeof status === "string" && /retir|shutdown|disabled|unavailable/i.test(status)) return false;
    const input = row.modalities?.input_modalities;
    const output = row.modalities?.output_modalities;
    if ((Array.isArray(input) && !input.includes("image"))
        || (Array.isArray(output) && !output.includes("video"))) return false;
    ids.add(id);
    return true;
  }).map((row) => ({ id: row.id.trim(), label: row.id.trim() }));
}

export async function requireAvailableVideoModel(config, signal) {
  if (config.provider !== "volcengine") return;
  if (signal?.aborted) throw new DOMException("aborted", "AbortError");
  if (!config.apiKey?.trim()) throw new Error("请先配置火山 API Key。");
  if (!config.model?.trim()) throw new Error("请选择或填写视频模型。");
  // Some deployments expose ep-* inference IDs but no compatible /models
  // listing. A failed/advisory list must not disable an otherwise valid task.
  if (isVolcanoModelRejected(config.apiKey, config.model)) {
    throw new Error("当前 API Key 无法调用此模型，请刷新模型列表或重新选择。");
  }
}
