"use strict";

/**
 * [Input] HachimoDock DSH bridge plugin loopback HTTP/SSE API (docs/dsh-integration-contract.md §2).
 * [Output] DshAdapter translating DSH health/sessions/inject frames into the shared AgentEvent shape.
 * [Pos] adapter node in agent-session-bus/src/adapters.
 * [Sync] If the DSH bridge contract changes, update this file and test/dsh-adapter.test.js.
 */

const http = require("node:http");
const https = require("node:https");

const { BaseAdapter } = require("./base");

const DEFAULT_BASE_URL = "http://127.0.0.1:17892";
const HEALTH_TIMEOUT_MS = 2000;
const SESSIONS_TIMEOUT_MS = 5000;

/**
 * DshAdapter — talks to the `dsh-hachimodock` Cordis plugin over 127.0.0.1.
 *
 * DSH owns its sessions and CLI; this adapter never spawns a process. It only
 * probes the plugin's HTTP API and forwards voice text through its SSE inject
 * stream, so any host/DSH version combination that runs the plugin works the
 * same way on the bus side.
 */
class DshAdapter extends BaseAdapter {
  /**
   * @param {object} [opts]
   * @param {(level: string, msg: string, details?: object) => void} [opts.log]
   * @param {NodeJS.ProcessEnv} [opts.env]   defaults to process.env
   * @param {string} [opts.baseUrl]          testing seam; overrides HACHIMODOCK_DSH_URL
   * @param {number} [opts.healthTimeoutMs]  testing seam; defaults to 2000
   */
  constructor({ log, env, baseUrl, healthTimeoutMs = HEALTH_TIMEOUT_MS } = {}) {
    super({ agentId: "deepseek-harness", log });
    this._env = env || process.env;
    this._baseUrl = resolveBaseUrl(baseUrl || this._env.HACHIMODOCK_DSH_URL);
    this._healthTimeoutMs = Number.isFinite(healthTimeoutMs) && healthTimeoutMs > 0
      ? healthTimeoutMs
      : HEALTH_TIMEOUT_MS;
  }

  /** Effective plugin URL, after HACHIMODOCK_DSH_URL/default resolution. */
  get baseUrl() {
    return this._baseUrl;
  }

  async isAvailable() {
    let response;
    try {
      response = await requestJson(this._baseUrl, "/health", { timeoutMs: this._healthTimeoutMs });
    } catch (error) {
      return {
        ready: false,
        reason: `无法连接 DSH 桥接插件（${this._baseUrl}）: ${describeError(error)}`,
      };
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
      return {
        ready: false,
        reason: `DSH 桥接插件 /health 返回 HTTP ${response.statusCode}`,
      };
    }
    let payload;
    try {
      payload = JSON.parse(response.body);
    } catch {
      return { ready: false, reason: "DSH 桥接插件 /health 未返回 JSON" };
    }
    if (!payload || payload.ok !== true) {
      const error = payload && typeof payload.error === "string" ? payload.error.trim() : "";
      return {
        ready: false,
        reason: error ? `DSH 桥接插件未就绪: ${error}` : "DSH 桥接插件未就绪",
      };
    }
    const version = Number(payload.version);
    return {
      ready: true,
      reason: null,
      ...(Number.isFinite(version) ? { version } : {}),
    };
  }

  async listSessions({ limit = 20 } = {}) {
    const safeLimit = Math.max(1, Math.min(200, Number.parseInt(limit, 10) || 20));
    let response;
    try {
      response = await requestJson(this._baseUrl, "/sessions", { timeoutMs: SESSIONS_TIMEOUT_MS });
    } catch (error) {
      this.log("warn", "DSH sessions request failed", { error: describeError(error) });
      return [];
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
      this.log("warn", "DSH sessions request failed", { status: response.statusCode });
      return [];
    }
    let payload;
    try {
      payload = JSON.parse(response.body);
    } catch {
      this.log("warn", "DSH sessions response was not JSON");
      return [];
    }
    const rows = payload && Array.isArray(payload.sessions) ? payload.sessions : [];
    return rows
      .map(normalizeSession)
      .filter(Boolean)
      .sort((left, right) => right.lastModified - left.lastModified)
      .slice(0, safeLimit);
  }

  async openNew({ cwd } = {}) {
    // DSH mints sessions inside the host, not here. Returning an empty id makes
    // the bus fall back to sessionId "auto", which the plugin maps to the
    // host's active session and reports back through the SSE done event.
    const ref = { id: "", lastModified: Date.now(), summary: "new (deepseek-harness)" };
    if (typeof cwd === "string" && cwd) ref.cwd = cwd;
    return ref;
  }

  async *inject({ sessionId, text, signal } = {}) {
    if (signal && signal.aborted) {
      yield cancelledEvent();
      return;
    }
    const requestedSessionId =
      typeof sessionId === "string" && sessionId.trim() ? sessionId.trim() : "auto";
    const body = JSON.stringify({
      sessionId: requestedSessionId,
      text: typeof text === "string" ? text : "",
    });

    let stream;
    try {
      stream = await openSseRequest(this._baseUrl, "/inject", body, signal);
    } catch (error) {
      if (signal && signal.aborted) {
        yield cancelledEvent();
      } else {
        yield {
          kind: "error",
          code: "AGENT_UNAVAILABLE",
          message: `DSH 注入请求失败: ${describeError(error)}`,
        };
      }
      return;
    }

    const { res } = stream;
    const statusCode = Number(res.statusCode) || 0;
    if (statusCode < 200 || statusCode >= 300) {
      const detail = await readResponseText(res);
      stream.destroy();
      yield {
        kind: "error",
        code: "AGENT_UNAVAILABLE",
        message: `DSH 注入请求返回 HTTP ${statusCode}${detail ? `: ${detail.slice(0, 200)}` : ""}`,
      };
      return;
    }

    let aborted = false;
    const onAbort = () => {
      aborted = true;
      stream.destroy();
    };
    if (signal) signal.addEventListener("abort", onAbort, { once: true });

    let sawTerminal = false;
    let buffer = "";
    try {
      res.setEncoding("utf8");
      for await (const chunk of res) {
        buffer = (buffer + chunk).replace(/\r\n?/g, "\n");
        let separator;
        while ((separator = buffer.indexOf("\n\n")) >= 0) {
          const block = buffer.slice(0, separator);
          buffer = buffer.slice(separator + 2);
          const frame = parseSseBlock(block);
          if (!frame) continue;
          const event = sseFrameToAgentEvent(frame, requestedSessionId);
          if (!event) continue;
          yield event;
          if (event.kind === "done" || event.kind === "error") {
            sawTerminal = true;
            break;
          }
        }
        if (sawTerminal) break;
      }
      if (!sawTerminal && buffer.trim()) {
        const frame = parseSseBlock(buffer);
        const event = frame ? sseFrameToAgentEvent(frame, requestedSessionId) : null;
        if (event) {
          sawTerminal = event.kind === "done" || event.kind === "error";
          yield event;
        }
      }
    } catch (error) {
      if (!aborted) {
        yield {
          kind: "error",
          code: "AGENT_STREAM_FAILED",
          message: `DSH 注入流中断: ${describeError(error)}`,
        };
        return;
      }
    } finally {
      if (signal) signal.removeEventListener("abort", onAbort);
      stream.destroy();
    }

    if (aborted) {
      if (!sawTerminal) yield cancelledEvent();
      return;
    }
    if (!sawTerminal) {
      yield {
        kind: "done",
        sessionId: requestedSessionId === "auto" ? "" : requestedSessionId,
        stopReason: "stream_ended",
      };
    }
  }
}

function resolveBaseUrl(raw) {
  const trimmed = typeof raw === "string" ? raw.trim() : "";
  if (!trimmed) return DEFAULT_BASE_URL;
  try {
    const url = new URL(trimmed);
    if (url.protocol !== "http:" && url.protocol !== "https:") return DEFAULT_BASE_URL;
    return url.toString().replace(/\/+$/, "");
  } catch {
    return DEFAULT_BASE_URL;
  }
}

function buildUrl(baseUrl, pathname) {
  return new URL(String(pathname || "").replace(/^\/+/, ""), `${baseUrl}/`);
}

function requestJson(baseUrl, pathname, { timeoutMs = SESSIONS_TIMEOUT_MS } = {}) {
  const url = buildUrl(baseUrl, pathname);
  const transport = url.protocol === "https:" ? https : http;
  const signal = AbortSignal.timeout(timeoutMs);
  return new Promise((resolve, reject) => {
    const req = transport.request(url, {
      method: "GET",
      headers: { accept: "application/json" },
      signal,
    }, (res) => {
      let body = "";
      res.setEncoding("utf8");
      res.on("data", (chunk) => { body += chunk; });
      res.on("end", () => resolve({ statusCode: Number(res.statusCode) || 0, body }));
      res.on("error", reject);
    });
    req.on("error", reject);
    req.end();
  });
}

function openSseRequest(baseUrl, pathname, body, signal) {
  const url = buildUrl(baseUrl, pathname);
  const transport = url.protocol === "https:" ? https : http;
  return new Promise((resolve, reject) => {
    const req = transport.request(url, {
      method: "POST",
      headers: {
        accept: "text/event-stream",
        "content-type": "application/json",
        "content-length": Buffer.byteLength(body),
        "cache-control": "no-cache",
      },
      signal,
    }, (res) => {
      resolve({
        res,
        destroy: () => {
          try { res.destroy(); } catch { /* ignore */ }
          try { req.destroy(); } catch { /* ignore */ }
        },
      });
    });
    req.on("error", reject);
    req.write(body);
    req.end();
  });
}

function readResponseText(res) {
  return new Promise((resolve) => {
    let text = "";
    res.setEncoding("utf8");
    res.on("data", (chunk) => { text += chunk; });
    res.on("end", () => resolve(text.trim()));
    res.on("error", () => resolve(text.trim()));
  });
}

/** @returns {{event: string, data: string} | null} */
function parseSseBlock(block) {
  let event = "message";
  const dataLines = [];
  for (const line of String(block || "").split("\n")) {
    if (!line || line.startsWith(":")) continue;
    const colon = line.indexOf(":");
    const field = colon === -1 ? line : line.slice(0, colon);
    let value = colon === -1 ? "" : line.slice(colon + 1);
    if (value.startsWith(" ")) value = value.slice(1);
    if (field === "event") event = value || "message";
    else if (field === "data") dataLines.push(value);
  }
  if (dataLines.length === 0) return null;
  return { event, data: dataLines.join("\n") };
}

function sseFrameToAgentEvent({ event, data }, requestedSessionId) {
  let payload;
  try {
    payload = JSON.parse(data);
  } catch {
    return null;
  }
  if (!payload || typeof payload !== "object") return null;

  switch (event) {
    case "token": {
      const text = typeof payload.text === "string" ? payload.text : "";
      return text ? { kind: "token", text } : null;
    }
    case "tool": {
      const name = typeof payload.name === "string" ? payload.name.trim() : "";
      if (!name) return null;
      const agentEvent = { kind: "tool", name, phase: payload.phase === "end" ? "end" : "start" };
      if ("input" in payload) agentEvent.input = payload.input;
      if (typeof payload.ok === "boolean") agentEvent.ok = payload.ok;
      return agentEvent;
    }
    case "done": {
      const sessionId = typeof payload.sessionId === "string" && payload.sessionId
        ? payload.sessionId
        : requestedSessionId === "auto" ? "" : requestedSessionId;
      const agentEvent = { kind: "done", sessionId };
      if (typeof payload.tokens === "number" && Number.isFinite(payload.tokens)) {
        agentEvent.tokens = payload.tokens;
      }
      if (typeof payload.stopReason === "string" && payload.stopReason) {
        agentEvent.stopReason = payload.stopReason;
      }
      return agentEvent;
    }
    case "error":
      return {
        kind: "error",
        code: typeof payload.code === "string" && payload.code ? payload.code : "DSH_ERROR",
        message: typeof payload.message === "string" && payload.message
          ? payload.message
          : "DSH 注入失败",
      };
    default:
      return null;
  }
}

function cancelledEvent() {
  return { kind: "error", code: "CANCELLED", message: "DSH inject aborted" };
}

function describeError(error) {
  if (!error) return "unknown error";
  if (error.name === "TimeoutError" || error.code === "ABORT_ERR") return "请求超时或已取消";
  return String(error.message || error);
}

/** @returns {import("../adapters/base").SessionRef | null} */
function normalizeSession(row) {
  if (!row || typeof row !== "object") return null;
  const id = typeof row.id === "string" ? row.id.trim() : "";
  if (!id) return null;
  const title = typeof row.title === "string" && row.title.trim()
    ? row.title.trim()
    : "DeepSeek Harness 会话";
  const lastModified = Number(row.lastModified);
  const session = {
    id,
    lastModified: Number.isFinite(lastModified) ? lastModified : 0,
    name: title,
    summary: title,
  };
  if (typeof row.cwd === "string" && row.cwd) session.cwd = row.cwd;
  if (typeof row.state === "string" && row.state.trim()) session.state = row.state.trim().toLowerCase();
  if (Number.isFinite(Number(row.transitionRevision)) && Number(row.transitionRevision) > 0) {
    session.transitionRevision = Number(row.transitionRevision);
  }
  return session;
}

module.exports = {
  DshAdapter,
  _internal: {
    resolveBaseUrl,
    parseSseBlock,
    sseFrameToAgentEvent,
    normalizeSession,
  },
};
