/**
 * [Input] per-family prompt + image bytes + Kling AI API config.
 * [Output] submitted task id, polled status, and downloaded MP4 bytes.
 * [Pos] provider node in ref/src/lib/avatar-pipeline/providers
 * [Sync] If this file changes, update this header. JWT signing uses Web Crypto HS256.
 */

import { downloadBinary, pipelineFetch, readJsonOrThrow, sleep, withRetry } from "../http.js";
import { uint8ToBase64 } from "../image.js";

const DEFAULT_BASE_URL = "https://api-beijing.klingai.com";
const TASK_PATH = "/v1/videos/image2video";
const DEFAULT_MODEL = "kling-v1-5";

function joinUrl(baseUrl, path) {
  const trimmed = (baseUrl || DEFAULT_BASE_URL).replace(/\/+$/, "");
  return path.startsWith("/") ? `${trimmed}${path}` : `${trimmed}/${path}`;
}

function base64UrlEncode(input) {
  // input: Uint8Array or string
  let bytes;
  if (typeof input === "string") {
    bytes = new TextEncoder().encode(input);
  } else {
    bytes = input;
  }
  const b64 = uint8ToBase64(bytes);
  return b64.replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

async function signJwtHS256(payload, secretKey) {
  const header = { alg: "HS256", typ: "JWT" };
  const segments = [
    base64UrlEncode(JSON.stringify(header)),
    base64UrlEncode(JSON.stringify(payload)),
  ];
  const signingInput = segments.join(".");
  const keyData = new TextEncoder().encode(secretKey);
  const key = await crypto.subtle.importKey(
    "raw",
    keyData,
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  const sigBuf = await crypto.subtle.sign("HMAC", key, new TextEncoder().encode(signingInput));
  segments.push(base64UrlEncode(new Uint8Array(sigBuf)));
  return segments.join(".");
}

async function buildAuthToken({ accessKey, secretKey, ttlSeconds = 1800 }) {
  const now = Math.floor(Date.now() / 1000);
  const payload = {
    iss: accessKey,
    exp: now + ttlSeconds,
    nbf: now - 5,
  };
  return signJwtHS256(payload, secretKey);
}

function extractTaskId(json) {
  return (
    json?.data?.task_id ||
    json?.data?.id ||
    json?.task_id ||
    json?.id ||
    ""
  );
}

function findVideoUrlInKlingResponse(json) {
  const videos = json?.data?.task_result?.videos || json?.task_result?.videos;
  if (Array.isArray(videos)) {
    for (const v of videos) {
      if (v?.url && /^https?:\/\//i.test(v.url)) return v.url;
    }
  }
  // fallback: deep walk
  function walk(obj) {
    if (!obj || typeof obj !== "object") return "";
    if (Array.isArray(obj)) {
      for (const item of obj) {
        const found = walk(item);
        if (found) return found;
      }
      return "";
    }
    for (const [k, v] of Object.entries(obj)) {
      if (typeof v === "string" && /url/i.test(k) && /^https?:\/\/.+/i.test(v)) return v;
      if (typeof v === "object") {
        const found = walk(v);
        if (found) return found;
      }
    }
    return "";
  }
  return walk(json);
}

function buildPayload({ model, mode, prompt, imageBase64, duration }) {
  const payload = {
    model_name: model || DEFAULT_MODEL,
    mode: mode || "standard",
    sound: "off",
    watermark_info: { enabled: false },
  };
  if (prompt) payload.prompt = prompt;
  if (imageBase64) {
    payload.image = imageBase64;
    payload.image_tail = imageBase64;
  }
  if (duration && String(duration).toLowerCase() !== "auto") {
    payload.duration = String(duration);
  }
  return payload;
}

const TERMINAL_OK = new Set(["succeed", "succeeded", "success"]);
const TERMINAL_FAIL = new Set(["failed", "error", "cancelled", "canceled"]);

async function submitTask({ accessKey, secretKey, baseUrl, model, mode, prompt, imageBase64, duration, signal }) {
  const token = await buildAuthToken({ accessKey, secretKey });
  const apiUrl = joinUrl(baseUrl, TASK_PATH);
  const payload = buildPayload({ model, mode, prompt, imageBase64, duration });
  const json = await withRetry(
    async () => {
      const response = await pipelineFetch(apiUrl, {
        method: "POST",
        signal,
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${token}`,
        },
        body: JSON.stringify(payload),
      });
      return readJsonOrThrow(response, "kling submit");
    },
    { retries: 3, signal },
  );
  const taskId = extractTaskId(json);
  if (!taskId) throw new Error(`kling submit: missing task_id: ${JSON.stringify(json).slice(0, 200)}`);
  return { taskId, raw: json };
}

async function queryTask({ accessKey, secretKey, baseUrl, taskId, signal }) {
  const token = await buildAuthToken({ accessKey, secretKey });
  const apiUrl = `${joinUrl(baseUrl, TASK_PATH)}/${encodeURIComponent(taskId)}`;
  return withRetry(
    async () => {
      const response = await pipelineFetch(apiUrl, {
        method: "GET",
        signal,
        headers: { Authorization: `Bearer ${token}` },
      });
      return readJsonOrThrow(response, "kling query");
    },
    { retries: 3, signal },
  );
}

// Same rationale as volcano.js — cap total wait so stuck-in-queue families
// fail cleanly and can be retried from AppearanceDetail.
const POLL_TIMEOUT_MS = 20 * 60 * 1000;

async function pollUntilTerminal({ accessKey, secretKey, baseUrl, taskId, signal, onPoll }) {
  const start = Date.now();
  let attempt = 0;
  while (true) {
    if (signal?.aborted) throw new DOMException("aborted", "AbortError");
    const elapsed = Date.now() - start;
    if (elapsed > POLL_TIMEOUT_MS) {
      throw new Error(
        `kling task ${taskId} timed out after ${Math.round(elapsed / 1000)}s (likely account-level queue). Retry this family from the detail page.`,
      );
    }
    const json = await queryTask({ accessKey, secretKey, baseUrl, taskId, signal });
    const status = String(json?.data?.task_status || json?.task_status || json?.status || "").toLowerCase();
    if (TERMINAL_OK.has(status)) {
      const videoUrl = findVideoUrlInKlingResponse(json);
      if (!videoUrl) throw new Error("kling task succeeded but no video URL was found");
      return { status, videoUrl, raw: json };
    }
    if (TERMINAL_FAIL.has(status)) {
      throw new Error(`kling task ${status}: ${JSON.stringify(json).slice(0, 300)}`);
    }
    onPoll?.({ status: status || "running", attempt, elapsedMs: elapsed });
    const intervalMs = elapsed < 30000 ? 5000 : 10000;
    await sleep(intervalMs, signal);
    attempt += 1;
  }
}

async function downloadVideo({ videoUrl, signal }) {
  // Use Rust-side download to bypass plugin-http Headers TypeError on CDN.
  return withRetry(() => downloadBinary(videoUrl, signal), { retries: 3, signal });
}

/**
 * Run one family end-to-end on Kling.
 *
 * @param {object} args
 * @param {{ accessKey: string, secretKey: string, baseUrl?: string, model?: string, mode?: string, duration?: string|number }} args.config
 * @param {string} args.prompt
 * @param {string} args.imageBase64   raw base64 (no data: prefix)
 * @param {AbortSignal} [args.signal]
 * @param {(event: { stage: 'submitting'|'polling'|'downloading'|'done', detail?: any }) => void} [args.onStage]
 */
export async function runKlingFamily({ config, prompt, imageBase64, signal, onStage }) {
  if (!config?.accessKey || !config?.secretKey) {
    throw new Error("Kling requires both accessKey and secretKey");
  }
  onStage?.({ stage: "submitting" });
  const { taskId, raw: submitRaw } = await submitTask({
    accessKey: config.accessKey,
    secretKey: config.secretKey,
    baseUrl: config.baseUrl,
    model: config.model,
    mode: config.mode,
    prompt,
    imageBase64,
    duration: config.duration,
    signal,
  });
  onStage?.({ stage: "polling", detail: { taskId } });
  const { videoUrl, raw: pollRaw } = await pollUntilTerminal({
    accessKey: config.accessKey,
    secretKey: config.secretKey,
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
