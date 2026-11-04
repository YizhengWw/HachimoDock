"use strict";

/**
 * [Input] HachimoDock DSH bridge plugin loopback HTTP/SSE API (docs/dsh-integration-contract.md §2).
 * [Output] DshAdapter translating DSH health/sessions/inject frames into the shared AgentEvent shape.
 * [Pos] adapter node in agent-session-bus/src/adapters.
 * [Sync] If the DSH bridge contract changes, update this file and test/dsh-adapter.test.js.
 */

const http = require("node:http");
const fs = require("node:fs");
const path = require("node:path");
const os = require("node:os");
const { randomUUID } = require("node:crypto");

const { windowsAcl, windowsPath } = require("./windows-acl");

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
    this._baseUrl = DEFAULT_BASE_URL; // Only the verified private descriptor can select an authenticated endpoint.
    this._capabilityFile = this._env.HACHIMODOCK_DSH_CAPABILITY_FILE || path.join(os.homedir(), '.claw-pet', 'dsh-bridge', 'capability.json');
    this._healthTimeoutMs = Number.isFinite(healthTimeoutMs) && healthTimeoutMs > 0
      ? healthTimeoutMs
      : HEALTH_TIMEOUT_MS;
  }

  /** Effective plugin URL, after HACHIMODOCK_DSH_URL/default resolution. */
  get baseUrl() {
    return this._baseUrl;
  }

  _auth() {
    const d = readCapability(this._capabilityFile);
    this._baseUrl = resolveBaseUrl(d.baseUrl);
    return { authorization: `Bearer ${d.tokens.bus}` };
  }

  async isAvailable() {
    let response;
    try {
      const auth = this._auth();
      response = await requestJson(this._baseUrl, "/sessions", { timeoutMs: this._healthTimeoutMs, headers: auth });
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
    if (!payload || !Array.isArray(payload.sessions) || payload.version !== 2) {
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
      const auth = this._auth();
      response = await requestJson(this._baseUrl, "/sessions", { timeoutMs: SESSIONS_TIMEOUT_MS, headers: auth });
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
    throw new Error("DSH requires an explicit existing session; no auto target")
  }

  async *inject({ sessionId, text, signal } = {}) {
    if (signal && signal.aborted) {
      yield cancelledEvent();
      return;
    }
    if (typeof sessionId !== 'string' || !sessionId || sessionId === 'auto' || sessionId.length > 200) {
      yield { kind: 'error', code: 'EXACT_SESSION_REQUIRED', message: 'DSH requires an explicit session' }; return;
    }
    const requestedSessionId = sessionId;
    const body = JSON.stringify({
      sessionId: requestedSessionId,
      requestId: randomUUID(),
      text: typeof text === "string" ? text : "",
    });

    let stream;
    try {
      const auth = this._auth();
      stream = await openSseRequest(this._baseUrl, "/inject", body, signal, auth);
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
        if (buffer.length > 65536) throw new Error("DSH SSE frame too large");
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
      yield { kind: 'error', code: 'DELIVERY_UNKNOWN', message: 'DSH stream ended without an admission receipt; no resend' };
    }
  }
}

function resolveBaseUrl(raw) {
  const u = new URL(raw || DEFAULT_BASE_URL);
  if (u.protocol !== 'http:' || u.hostname !== '127.0.0.1' || !u.port || u.username || u.password || u.pathname !== '/' || u.search || u.hash) throw new Error('DSH requires an exact loopback endpoint');
  return u.origin;
}
function readCapability(file) {
  const check = (st, dir) => {
    if (st.isSymbolicLink() || (dir ? !st.isDirectory() : !st.isFile()) || process.platform !== 'win32' && ((st.mode & 0o077) || process.getuid && st.uid !== process.getuid())) throw new Error('unsafe DSH capability');
  };
  const win = process.platform === 'win32';
  if (win) { file = windowsPath(file); windowsAcl('verify-file', file); }
  check(fs.lstatSync(path.dirname(file)), true);
  check(fs.lstatSync(file), false);
  const fd = fs.openSync(file, fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW || 0));
  try {
    const st = fs.fstatSync(fd); check(st, false);
    if (st.size > 4096) throw new Error('invalid DSH capability');
    if (win) windowsAcl('verify-file', file);
    const d = JSON.parse(fs.readFileSync(fd, 'utf8'));
    resolveBaseUrl(d.baseUrl);
    if (d.version !== 2 || !/^[a-f0-9]{64}$/.test(d.epoch) || !/^[a-f0-9]{64}$/.test(d.tokens?.bus)) throw new Error('invalid DSH capability');
    return d;
  } finally { fs.closeSync(fd); }
}

function buildUrl(baseUrl, pathname) {
  return new URL(String(pathname || "").replace(/^\/+/, ""), `${baseUrl}/`);
}

function requestJson(baseUrl, pathname, { timeoutMs = SESSIONS_TIMEOUT_MS, headers = {} } = {}) {
  const url = buildUrl(baseUrl, pathname);
  const transport = http;
  const signal = AbortSignal.timeout(timeoutMs);
  return new Promise((resolve, reject) => {
    const req = transport.request(url, {
      method: "GET",
      headers: { accept: "application/json", ...headers },
      signal,
    }, (res) => {
      let body = "";
      res.setEncoding("utf8");
      res.on("data", (chunk) => { body += chunk; if (body.length > 65536) { res.destroy(); reject(new Error("DSH response too large")); } });
      res.on("end", () => resolve({ statusCode: Number(res.statusCode) || 0, body }));
      res.on("error", reject);
    });
    req.on("error", reject);
    req.end();
  });
}

function openSseRequest(baseUrl, pathname, body, signal, headers = {}) {
  const deadline = AbortSignal.timeout(600000);
  signal = signal ? AbortSignal.any([signal, deadline]) : deadline;
  const url = buildUrl(baseUrl, pathname);
  const transport = http;
  return new Promise((resolve, reject) => {
    const req = transport.request(url, {
      method: "POST",
      headers: {
        ...headers,
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
    req.setTimeout(10000, () => req.destroy(new Error("DSH stream idle timeout")));
    req.on("error", reject);
    req.write(body);
    req.end();
  });
}

function readResponseText(res) {
  return new Promise((resolve) => {
    let text = "";
    res.setEncoding("utf8");
    res.on("data", (chunk) => { text += chunk; if (text.length > 65536) res.destroy(); });
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
      if (payload.sessionId !== requestedSessionId) return { kind: "error", code: "SESSION_MISMATCH", message: "DSH receipt session mismatch" };
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
  if (typeof row.displayContent === "string" && row.displayContent.trim()) {
    session.displayContent = row.displayContent.trim();
  }
  if (typeof row.state === "string" && row.state.trim()) session.state = row.state.trim().toLowerCase();
  const status = ['running', 'idle', 'unknown'].includes(row.status) ? row.status : 'unknown';
  const active = status === 'running' && row.active === true ? true
    : status === 'idle' && row.active === false ? false : null;
  session.status = active === null ? 'unknown' : status;
  session.active = active;
  session.stateDetailUnknown = row.stateDetailUnknown !== false || ![
    'working', 'thinking', 'tool_running', 'waiting_user', 'done', 'error', 'stopped',
  ].includes(session.state);
  session.activitySource = active !== null && row.activitySource === 'live-agent-status'
    ? 'live-agent-status' : null;
  session.waitKind = session.state === 'waiting_user' && ['question', 'toolapproval'].includes(row.waitKind)
    ? row.waitKind : null;
  if (typeof row.epoch === 'string' && /^[a-f0-9]{64}$/i.test(row.epoch)) session.epoch = row.epoch;

  if (Number.isFinite(Number(row.transitionRevision)) && Number(row.transitionRevision) > 0) {
    session.transitionRevision = Number(row.transitionRevision);
  }
  return session;
}

module.exports = {
  DshAdapter,
  _internal: {
    resolveBaseUrl,
    readCapability,
    parseSseBlock,
    sseFrameToAgentEvent,
    normalizeSession,
  },
};
