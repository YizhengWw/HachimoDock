/**
 * [Input] per-family prompt + image bytes + user-defined OpenAI-style API config.
 * [Output] submitted task id, polled status, and downloaded MP4 bytes.
 * [Pos] provider node in ref/src/lib/avatar-pipeline/providers
 * [Sync] If this file changes, update this header. Supports the M06.3 "custom provider" advanced settings.
 */

import { downloadBinary, pipelineFetch, readJsonOrThrow, sleep, withRetry } from "../http.js";

function joinUrl(baseUrl, path) {
  const trimmed = (baseUrl || "").replace(/\/+$/, "");
  return path.startsWith("/") ? `${trimmed}${path}` : `${trimmed}/${path}`;
}

function buildAuthHeaders({ apiKey, advanced }) {
  const headerName = advanced?.authHeader?.trim() || "Authorization";
  const prefix = advanced?.authPrefix?.trim();
  const value = prefix ? `${prefix} ${apiKey}` : apiKey;
  return { [headerName]: value };
}

function pickValueByPath(obj, path) {
  if (!path) return "";
  const parts = path.split(/\.|\[(\d+)\]/).filter(Boolean);
  let cur = obj;
  for (const part of parts) {
    if (cur == null) return "";
    if (/^\d+$/.test(part)) cur = cur[Number(part)];
    else cur = cur[part];
  }
  return cur;
}

function extractTaskId(json) {
  return json?.data?.task_id || json?.id || json?.task_id || json?.data?.id || "";
}

function inferStatus(json) {
  return String(
    json?.data?.status ||
      json?.status ||
      json?.data?.task_status ||
      json?.task_status ||
      "",
  ).toLowerCase();
}

const TERMINAL_OK = new Set(["succeeded", "success", "done", "completed", "complete", "succeed"]);
const TERMINAL_FAIL = new Set(["failed", "error", "cancelled", "canceled"]);

/**
 * Run one family end-to-end on a user-defined OpenAI-style provider.
 *
 * @param {object} args
 * @param {{
 *   apiKey: string,
 *   baseUrl: string,
 *   model: string,
 *   advanced?: {
 *     authHeader?: string,
 *     authPrefix?: string,
 *     createPath?: string,
 *     queryPath?: string,
 *     timeoutMs?: number,
 *     pollingIntervalMs?: number,
 *     resultPath?: string,
 *   }
 * }} args.config
 * @param {string} args.prompt
 * @param {string} args.imageDataUrl
 * @param {AbortSignal} [args.signal]
 * @param {(event: { stage: 'submitting'|'polling'|'downloading'|'done', detail?: any }) => void} [args.onStage]
 */
export async function runCustomFamily({ config, prompt, imageDataUrl, signal, onStage }) {
  if (!config?.apiKey) throw new Error("Custom provider requires apiKey");
  if (!config?.baseUrl) throw new Error("Custom provider requires baseUrl");
  if (!config?.model) throw new Error("Custom provider requires model");

  const advanced = config.advanced || {};
  const createPath = advanced.createPath || "/v1/video/generations";
  const queryPath = advanced.queryPath || "/v1/tasks/{id}";
  const resultPath = advanced.resultPath || "data[0].url";
  const pollIntervalMs = Number(advanced.pollingIntervalMs) || 3000;
  const timeoutMs = Number(advanced.timeoutMs) || 120000;

  const headers = {
    "Content-Type": "application/json",
    ...buildAuthHeaders({ apiKey: config.apiKey, advanced }),
  };

  // Submit
  onStage?.({ stage: "submitting" });
  const submitUrl = joinUrl(config.baseUrl, createPath);
  const payload = {
    model: config.model,
    prompt,
    image: imageDataUrl,
  };
  const submitJson = await withRetry(
    async () => {
      const response = await pipelineFetch(submitUrl, {
        method: "POST",
        signal,
        headers,
        body: JSON.stringify(payload),
      });
      return readJsonOrThrow(response, "custom submit");
    },
    { retries: 3, signal },
  );

  const taskId = extractTaskId(submitJson);
  let videoUrl = "";

  if (!taskId) {
    // Some providers respond synchronously with the video URL on submit.
    const raw = pickValueByPath(submitJson, resultPath);
    if (typeof raw === "string" && /^https?:\/\//i.test(raw)) {
      videoUrl = raw;
    } else {
      throw new Error(`custom submit: missing task id and no direct video URL at ${resultPath}`);
    }
  } else {
    onStage?.({ stage: "polling", detail: { taskId } });
    const start = Date.now();
    let attempt = 0;
    while (!videoUrl) {
      if (signal?.aborted) throw new DOMException("aborted", "AbortError");
      if (Date.now() - start > timeoutMs) {
        throw new Error(`custom provider: timed out after ${timeoutMs}ms waiting for task ${taskId}`);
      }
      const queryUrl = joinUrl(config.baseUrl, queryPath.replace("{id}", encodeURIComponent(taskId)));
      const queryJson = await withRetry(
        async () => {
          const response = await pipelineFetch(queryUrl, { method: "GET", signal, headers });
          return readJsonOrThrow(response, "custom query");
        },
        { retries: 3, signal },
      );
      const status = inferStatus(queryJson);
      if (TERMINAL_FAIL.has(status)) {
        throw new Error(`custom task ${status}: ${JSON.stringify(queryJson).slice(0, 300)}`);
      }
      if (TERMINAL_OK.has(status) || resultPath) {
        const url = pickValueByPath(queryJson, resultPath);
        if (typeof url === "string" && /^https?:\/\//i.test(url)) {
          videoUrl = url;
          break;
        }
      }
      onStage?.({ stage: "polling", detail: { taskId, status, attempt } });
      await sleep(pollIntervalMs, signal);
      attempt += 1;
    }
  }

  onStage?.({ stage: "downloading", detail: { videoUrl } });
  // Rust-side download bypasses plugin-http Headers TypeError on CDN responses.
  const videoBytes = await withRetry(() => downloadBinary(videoUrl, signal), {
    retries: 3,
    signal,
  });
  onStage?.({ stage: "done", detail: { taskId } });
  return {
    taskId: taskId || "",
    videoBytes,
    videoUrl,
    raw: { submit: submitJson },
  };
}
