/**
 * [Input] per-family prompt + image payload + Volcengine Ark API-key/model-name config.
 * [Output] submitted task id, model-specific Ark v3 first/last-frame payload,
 *          account-actionable submit errors, polled status, and downloaded MP4 bytes.
 * [Pos] provider node in ref/src/lib/avatar-pipeline/providers
 * [Sync] If this file changes, update this header.
 */

import { downloadBinary, pipelineFetch, readJsonOrThrow, sleep, withRetry } from "../http.js";

export const DEFAULT_VOLCANO_BASE_URL = "https://ark.cn-beijing.volces.com";
export const VOLCANO_TASK_PATH = "/api/v3/contents/generations/tasks";
export const DEFAULT_VOLCANO_VIDEO_MODEL = "doubao-seedance-2-0-260128";
const VOLCANO_FALLBACK_VIDEO_MODEL = "doubao-seedance-1-5-pro-251215";

function joinUrl(baseUrl, path) {
  const trimmed = (baseUrl || DEFAULT_VOLCANO_BASE_URL).replace(/\/+$/, "");
  return path.startsWith("/") ? `${trimmed}${path}` : `${trimmed}/${path}`;
}

function normalizeMediaEntry(type, value, role) {
  if (!value) return null;
  if (typeof value === "object" && value.type) return value;
  const url = typeof value === "string" ? value : value.url;
  if (!url) return null;
  const urlKey = type === "video_url" ? "video_url" : type === "audio_url" ? "audio_url" : "image_url";
  const entry = { type, [urlKey]: { url } };
  const resolvedRole = role || (typeof value === "object" ? value.role : "");
  if (resolvedRole) entry.role = resolvedRole;
  return entry;
}

function isSeedance15Model(model) {
  return /doubao-seedance-1-5/i.test(String(model || ""));
}

function hasPromptFlag(prompt, flag) {
  return new RegExp(`(^|\\s)--${flag}(\\s|$)`, "i").test(String(prompt || ""));
}

function buildSeedance15Prompt(prompt, { duration, watermark }) {
  const parts = [String(prompt || "").trim()];
  if (!hasPromptFlag(prompt, "duration")) {
    const normalizedDuration =
      duration !== undefined && duration !== null && String(duration).toLowerCase() !== "auto"
        ? duration
        : 5;
    parts.push(`--duration ${normalizedDuration}`);
  }
  if (!hasPromptFlag(prompt, "camerafixed")) {
    parts.push("--camerafixed false");
  }
  if (!hasPromptFlag(prompt, "watermark")) {
    parts.push(`--watermark ${Boolean(watermark)}`);
  }
  return parts.filter(Boolean).join(" ");
}

export function buildVolcanoTaskPayload({
  model,
  prompt,
  imageDataUrl,
  contentItems,
  referenceImages = [],
  referenceVideos = [],
  referenceAudios = [],
  duration,
  ratio,
  resolution,
  generateAudio = false,
  watermark = false,
}) {
  if (isSeedance15Model(model)) {
    return {
      model: model || DEFAULT_VOLCANO_VIDEO_MODEL,
      content: [
        { type: "text", text: buildSeedance15Prompt(prompt, { duration, watermark }) },
        normalizeMediaEntry("image_url", imageDataUrl, "first_frame"),
        normalizeMediaEntry("image_url", imageDataUrl, "last_frame"),
      ].filter(Boolean),
    };
  }

  const content = Array.isArray(contentItems)
    ? [...contentItems]
    : [
        { type: "text", text: prompt },
        normalizeMediaEntry("image_url", imageDataUrl, "first_frame"),
        normalizeMediaEntry("image_url", imageDataUrl, "last_frame"),
        ...referenceImages.map((item) => normalizeMediaEntry("image_url", item, "reference_image")),
        ...referenceVideos.map((item) => normalizeMediaEntry("video_url", item, "reference_video")),
        ...referenceAudios.map((item) => normalizeMediaEntry("audio_url", item, "reference_audio")),
      ].filter(Boolean);

  const payload = {
    model: model || DEFAULT_VOLCANO_VIDEO_MODEL,
    content,
    generate_audio: Boolean(generateAudio),
    watermark: Boolean(watermark),
  };
  if (duration !== undefined && duration !== null && String(duration).toLowerCase() !== "auto") {
    const num = Number(duration);
    payload.duration = Number.isFinite(num) ? num : duration;
  }
  if (ratio) payload.ratio = ratio;
  if (resolution) payload.resolution = resolution;
  return payload;
}

function summarizeUrl(url) {
  if (!url) return { kind: "empty" };
  if (/^data:/i.test(url)) {
    const match = /^data:([^;,]+)?(?:;base64)?,/i.exec(url);
    return {
      kind: "data_url",
      mime: match?.[1] || "",
      length: url.length,
    };
  }
  try {
    const parsed = new URL(url);
    return {
      kind: "remote_url",
      origin: parsed.origin,
      pathname: parsed.pathname,
      hasQuery: parsed.search.length > 0,
    };
  } catch {
    return { kind: "unknown_url", length: String(url).length };
  }
}

export function buildVolcanoTaskDiagnostics(payload) {
  return {
    model: payload?.model || "",
    duration: payload?.duration,
    ratio: payload?.ratio,
    resolution: payload?.resolution,
    generate_audio: payload?.generate_audio,
    watermark: payload?.watermark,
    content: Array.isArray(payload?.content)
      ? payload.content.map((part) => ({
          type: part?.type || "",
          role: part?.role || "",
          textLength: typeof part?.text === "string" ? part.text.length : undefined,
          url: summarizeUrl(part?.image_url?.url || part?.video_url?.url || part?.audio_url?.url || ""),
        }))
      : [],
  };
}

function parseVolcanoErrorEnvelope(message) {
  const raw = String(message || "");
  const jsonStart = raw.indexOf("{");
  if (jsonStart < 0) return null;
  try {
    return JSON.parse(raw.slice(jsonStart));
  } catch {
    return null;
  }
}

export function normalizeVolcanoSubmitErrorMessage(message, model) {
  const raw = String(message || "");
  const envelope = parseVolcanoErrorEnvelope(raw);
  const code = String(envelope?.error?.code || "");
  const providerMessage = String(envelope?.error?.message || "");
  if (code === "ModelNotOpen" || /has not activated the model/i.test(providerMessage)) {
    const selectedModel = String(model || "").trim() || "所选模型";
    return [
      `火山引擎模型未开通：当前 Ark 账号还没有开通 ${selectedModel}。`,
      `请在 Ark 控制台开通该模型服务，或先在模型下拉中改用 ${VOLCANO_FALLBACK_VIDEO_MODEL}。`,
      `原始错误：${raw}`,
    ].join("\n");
  }
  return raw;
}

function shouldRetryVolcanoHttp(err) {
  const message = String(err?.message || err || "");
  const match = /HTTP\s+(\d{3})/.exec(message);
  if (!match) return true;
  const status = Number(match[1]);
  return status === 408 || status === 409 || status === 425 || status === 429 || status >= 500;
}

function extractTaskId(json) {
  return json?.data?.task_id || json?.id || json?.task_id || "";
}

function looksLikeVideoUrl(v) {
  return typeof v === "string" && /^https?:\/\/.+/i.test(v);
}

const URL_KEY_CANDIDATES = new Set([
  "video_url",
  "video_url_url",
  "videoUrl",
  "output_url",
  "video",
  "url",
  "result_url",
  "file_url",
]);

function findVideoUrl(obj) {
  if (!obj || typeof obj !== "object") return "";
  if (Array.isArray(obj)) {
    for (const item of obj) {
      const found = findVideoUrl(item);
      if (found) return found;
    }
    return "";
  }
  for (const [k, v] of Object.entries(obj)) {
    if (URL_KEY_CANDIDATES.has(k) && looksLikeVideoUrl(v)) return v;
    if (typeof v === "string" && (k.toLowerCase().includes("url") || k.toLowerCase().includes("file")) && looksLikeVideoUrl(v)) {
      return v;
    }
    if (typeof v === "object") {
      const found = findVideoUrl(v);
      if (found) return found;
    }
  }
  return "";
}

const TERMINAL_OK = new Set(["succeeded", "success", "done", "completed", "complete"]);
const TERMINAL_FAIL = new Set(["failed", "error", "cancelled", "canceled"]);
const STILL_RUNNING = new Set(["running", "pending", "processing", "queued", "waiting"]);

async function submitTask({
  apiKey,
  baseUrl,
  model,
  prompt,
  imageDataUrl,
  duration,
  ratio,
  resolution,
  generateAudio,
  watermark,
  signal,
}) {
  const apiUrl = joinUrl(baseUrl, VOLCANO_TASK_PATH);
  const payload = buildVolcanoTaskPayload({
    model,
    prompt,
    imageDataUrl,
    duration,
    ratio,
    resolution,
    generateAudio,
    watermark,
  });
  const json = await withRetry(
    async () => {
      const response = await pipelineFetch(apiUrl, {
        method: "POST",
        signal,
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${apiKey}`,
        },
        body: JSON.stringify(payload),
      });
      try {
        return await readJsonOrThrow(response, "volcano submit");
      } catch (err) {
        const diagnostics = JSON.stringify(buildVolcanoTaskDiagnostics(payload));
        const message = normalizeVolcanoSubmitErrorMessage(err?.message || String(err), model);
        throw new Error(`${message}\nVolcano payload summary: ${diagnostics}`, {
          cause: err,
        });
      }
    },
    { retries: 3, signal, shouldRetry: shouldRetryVolcanoHttp },
  );
  const taskId = extractTaskId(json);
  if (!taskId) throw new Error(`volcano submit: missing task_id in response: ${JSON.stringify(json).slice(0, 200)}`);
  return { taskId, raw: json };
}

async function queryTask({ apiKey, baseUrl, taskId, signal }) {
  const apiUrl = `${joinUrl(baseUrl, VOLCANO_TASK_PATH)}/${encodeURIComponent(taskId)}`;
  return withRetry(
    async () => {
      const response = await pipelineFetch(apiUrl, {
        method: "GET",
        signal,
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${apiKey}`,
        },
      });
      return readJsonOrThrow(response, "volcano query");
    },
    { retries: 3, signal, shouldRetry: shouldRetryVolcanoHttp },
  );
}

// Cap total polling time so a family stuck in Volcano's server-side queue
// eventually fails instead of sitting in "生成中" forever. User can retry from
// AppearanceDetail via runSingleFamilyRetry. 20 min covers normal generation
// (~2-5 min) plus some queue headroom; anything longer is almost always the
// account-level concurrency limit.
const POLL_TIMEOUT_MS = 20 * 60 * 1000;

async function pollUntilTerminal({ apiKey, baseUrl, taskId, signal, onPoll }) {
  const start = Date.now();
  let attempt = 0;
  while (true) {
    if (signal?.aborted) throw new DOMException("aborted", "AbortError");
    const elapsed = Date.now() - start;
    if (elapsed > POLL_TIMEOUT_MS) {
      throw new Error(
        `volcano task ${taskId} timed out after ${Math.round(elapsed / 1000)}s (likely account-level queue). Retry this family from the detail page once others finish.`,
      );
    }
    const json = await queryTask({ apiKey, baseUrl, taskId, signal });
    const status = String(json?.status || json?.data?.status || json?.task?.status || "").toLowerCase();
    const videoUrl = findVideoUrl(json);
    if (TERMINAL_OK.has(status)) {
      if (!videoUrl) throw new Error("volcano task succeeded but no video URL was found");
      return { status, videoUrl, raw: json };
    }
    if (TERMINAL_FAIL.has(status)) {
      throw new Error(`volcano task ${status}: ${JSON.stringify(json).slice(0, 300)}`);
    }
    if (!STILL_RUNNING.has(status) && status !== "") {
      // unknown status: keep polling for a bit but cap attempts
      if (attempt > 60) throw new Error(`volcano task unknown status: ${status}`);
    }
    onPoll?.({ status: status || "running", attempt, elapsedMs: elapsed });
    const intervalMs = elapsed < 30000 ? 5000 : 10000;
    await sleep(intervalMs, signal);
    attempt += 1;
  }
}

async function downloadVideo({ videoUrl, signal }) {
  // Use Rust-side download to bypass plugin-http Headers TypeError on TOS CDN.
  return withRetry(() => downloadBinary(videoUrl, signal), { retries: 3, signal });
}

/**
 * Run one family end-to-end on Volcano.
 *
 * @param {object} args
 * @param {{ apiKey: string, baseUrl?: string, model?: string, duration?: string|number, ratio?: string, resolution?: string, generateAudio?: boolean, watermark?: boolean }} args.config
 * @param {string} args.prompt
 * @param {string} args.imageDataUrl
 * @param {AbortSignal} [args.signal]
 * @param {(event: { stage: 'submitting'|'polling'|'downloading'|'done', detail?: any }) => void} [args.onStage]
 * @returns {Promise<{ taskId: string, videoBytes: Uint8Array, videoUrl: string, raw: object }>}
 */
export async function runVolcanoFamily({ config, prompt, imageDataUrl, signal, onStage }) {
  if (!config?.apiKey) throw new Error("Volcano API key is required");
  onStage?.({ stage: "submitting" });
  const { taskId, raw: submitRaw } = await submitTask({
    apiKey: config.apiKey,
    baseUrl: config.baseUrl,
    model: config.model,
    prompt,
    imageDataUrl,
    duration: config.duration,
    ratio: config.ratio,
    resolution: config.resolution,
    generateAudio: config.generateAudio,
    watermark: config.watermark,
    signal,
  });
  onStage?.({ stage: "polling", detail: { taskId } });
  const { videoUrl, raw: pollRaw } = await pollUntilTerminal({
    apiKey: config.apiKey,
    baseUrl: config.baseUrl,
    taskId,
    signal,
    onPoll: (e) => onStage?.({ stage: "polling", detail: { taskId, ...e } }),
  });
  onStage?.({ stage: "downloading", detail: { taskId, videoUrl } });
  const videoBytes = await downloadVideo({ videoUrl, signal });
  onStage?.({ stage: "done", detail: { taskId } });
  return { taskId, videoBytes, videoUrl, raw: { submit: submitRaw, poll: pollRaw } };
}
