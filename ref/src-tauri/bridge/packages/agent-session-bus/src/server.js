"use strict";

/*
 * [Input] Agent adapters, session-status providers, and HTTP inject/list requests.
 * [Output] Session snapshots, cursor-based ordered lifecycle events, and streamed Agent injections that drain adapter cleanup before closing SSE.
 * [Pos] HTTP boundary for Agent Session Bus.
 */

const crypto = require("crypto");
const http = require("http");
const { isInternalCodexSession } = require("./util/codex-session-filter");

const {
  openSseStream,
  writeSseEvent,
  endSseStream,
  startSseHeartbeat,
} = require("./sse");

const MAX_REQUEST_BYTES = 64 * 1024;
const ACTIVE_SESSION_STATES = new Set([
  "working",
  "thinking",
  "tool_running",
  "speaking",
  "waiting_user",
]);
const ACTIVE_ONLY_DISPLAY_CONTENT = /^(正在处理|正在思考|正在执行|正在回复)/;

function normalizeSessionDisplayContent(state, value) {
  const content = typeof value === "string" ? value.trim() : "";
  if (!content || ACTIVE_SESSION_STATES.has(state) || !ACTIVE_ONLY_DISPLAY_CONTENT.test(content)) {
    return content;
  }
  if (state === "done") return "已完成";
  if (state === "error") return "执行失败";
  return "";
}

function readJsonBody(req) {
  return new Promise((resolve, reject) => {
    let total = 0;
    const chunks = [];
    req.on("data", (chunk) => {
      total += chunk.length;
      if (total > MAX_REQUEST_BYTES) {
        reject(Object.assign(new Error("payload too large"), { code: "PAYLOAD_TOO_LARGE", statusCode: 413 }));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on("end", () => {
      try {
        const raw = Buffer.concat(chunks).toString("utf8");
        if (!raw.trim()) {
          resolve({});
          return;
        }
        resolve(JSON.parse(raw));
      } catch (error) {
        reject(Object.assign(new Error("invalid JSON body"), { code: "INVALID_JSON", statusCode: 400, cause: error }));
      }
    });
    req.on("error", reject);
  });
}

// Browser pages can send simple requests to loopback even when they cannot
// read the response. Only the app webview and local development UI may attach
// an Origin; native sidecars and CLI clients normally send no Origin.
const ALLOWED_REQUEST_HEADERS = "content-type, accept, x-requested-with";
const ALLOWED_METHODS = "GET, POST, OPTIONS";
const ALLOWED_ORIGINS = new Set([
  "tauri://localhost",
  "http://tauri.localhost",
  "https://tauri.localhost",
  "http://localhost:4173",
  "http://127.0.0.1:4173",
]);
function isTrustedOrigin(req) {
  const origin = req.headers.origin;
  return !origin || ALLOWED_ORIGINS.has(origin);
}
function corsHeaders(req) {
  const origin = req.headers.origin;
  if (!origin || !ALLOWED_ORIGINS.has(origin)) return {};
  return {
    "Access-Control-Allow-Origin": origin,
    Vary: "Origin",
    "Access-Control-Allow-Methods": ALLOWED_METHODS,
    "Access-Control-Allow-Headers": ALLOWED_REQUEST_HEADERS,
    // Expose so EventSource-style readers can see SSE control headers.
    "Access-Control-Expose-Headers": "content-type",
    "Access-Control-Max-Age": "600",
  };
}

function sendJson(req, res, status, body) {
  if (res.headersSent || res.writableEnded) return;
  const payload = Buffer.from(JSON.stringify(body), "utf8");
  res.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": String(payload.length),
    ...corsHeaders(req),
  });
  res.end(payload);
}

function sendNoContent(req, res, status = 204) {
  if (res.headersSent || res.writableEnded) return;
  res.writeHead(status, corsHeaders(req));
  res.end();
}

class AgentSessionBus {
  /**
   * @param {object} opts
   * @param {number} [opts.port]
   * @param {string} [opts.host]
   * @param {import("./registry").AdapterRegistry} opts.registry
   * @param {(agentId: string) => Array<object>|Promise<Array<object>>} [opts.sessionStatusProvider]
   * @param {(agentId: string, options: object) => object|Promise<object>} [opts.sessionEventProvider]
   * @param {(level: string, msg: string, details?: object) => void} [opts.log]
   */
  constructor({
    port = 8181,
    host = "127.0.0.1",
    registry,
    sessionStatusProvider,
    sessionEventProvider,
    log,
  } = {}) {
    if (!registry) throw new Error("AgentSessionBus requires a registry");
    this.port = port;
    this.host = host;
    this.registry = registry;
    this.sessionStatusProvider = typeof sessionStatusProvider === "function"
      ? sessionStatusProvider
      : null;
    this.sessionEventProvider = typeof sessionEventProvider === "function"
      ? sessionEventProvider
      : null;
    this.log = log || (() => {});
    this._server = null;
    this._actualPort = null;
    /** @type {Map<string, {abort: () => void, agentId: string, sessionId: string, startedAt: number}>} */
    this._activeRuns = new Map();
  }

  start() {
    if (this._server) {
      throw new Error("AgentSessionBus already started");
    }
    return new Promise((resolve, reject) => {
      const server = http.createServer((req, res) => this._handle(req, res));
      const onError = (err) => {
        server.removeListener("listening", onListening);
        reject(err);
      };
      const onListening = () => {
        server.removeListener("error", onError);
        const addr = server.address();
        this._actualPort = addr && typeof addr === "object" ? addr.port : this.port;
        this._server = server;
        this.log("info", "bus listening", { host: this.host, port: this._actualPort, adapters: this.registry.ids() });
        resolve(this._actualPort);
      };
      server.once("error", onError);
      server.once("listening", onListening);
      server.listen(this.port, this.host);
    });
  }

  async stop() {
    for (const [, run] of this._activeRuns) {
      try { run.abort(); } catch { /* ignore */ }
    }
    this._activeRuns.clear();
    if (!this._server) return;
    const server = this._server;
    this._server = null;
    await new Promise((resolve) => server.close(() => resolve()));
    this.log("info", "bus stopped", {});
  }

  /** @returns {number|null} */
  port_() {
    return this._actualPort;
  }

  async _handle(req, res) {
    const url = new URL(req.url, `http://${req.headers.host || `${this.host}:${this._actualPort || this.port}`}`);
    if (!isTrustedOrigin(req)) {
      sendJson(req, res, 403, { ok: false, error: "untrusted browser origin", code: "UNTRUSTED_ORIGIN" });
      return;
    }
    // Short-circuit CORS preflights before any route dispatch — every fetch
    // from the Tauri webview (`tauri://localhost`) issues an OPTIONS first
    // for non-simple requests (e.g. POST application/json).
    if (req.method === "OPTIONS") {
      sendNoContent(req, res, 204);
      return;
    }
    const route = `${req.method} ${url.pathname}`;
    try {
      switch (route) {
        case "GET /agent/health":
          sendJson(req, res, 200, { ok: true, adapters: this.registry.ids(), activeRuns: this._activeRuns.size });
          return;
        case "GET /agent/status":
          await this._handleStatus(req, res, url);
          return;
        case "GET /agent/sessions":
          await this._handleSessions(req, res, url);
          return;
        case "GET /agent/session-events":
          await this._handleSessionEvents(req, res, url);
          return;
        case "POST /agent/inject":
          await this._handleInject(req, res, url);
          return;
        case "POST /agent/cancel":
          await this._handleCancel(req, res, url);
          return;
        default:
          sendJson(req, res, 404, { ok: false, error: `no route ${route}` });
          return;
      }
    } catch (error) {
      const status = Number.isFinite(error?.statusCode) ? error.statusCode : 500;
      this.log("error", "request failed", {
        route,
        error: String(error && error.stack ? error.stack : error),
      });
      if (!res.headersSent) {
        sendJson(req, res, status, { ok: false, error: error?.message || "internal error", code: error?.code || "INTERNAL" });
      } else if (!res.writableEnded) {
        try { res.end(); } catch { /* ignore */ }
      }
    }
  }

  async _handleStatus(req, res, _url) {
    const adapters = await this.registry.statusAll({ fresh: false });
    sendJson(req, res, 200, { ok: true, adapters });
  }

  async _handleSessions(req, res, url) {
    const agentId = url.searchParams.get("agentId");
    const limit = Number.parseInt(url.searchParams.get("limit") || "20", 10);
    if (!agentId) {
      sendJson(req, res, 400, { ok: false, error: "agentId is required" });
      return;
    }
    const adapter = this.registry.get(agentId);
    if (!adapter) {
      sendJson(req, res, 404, { ok: false, error: `unknown agentId: ${agentId}` });
      return;
    }
    const probe = await this.registry.statusOne(agentId);
    let sessions = [];
    const safeLimit = Number.isFinite(limit) && limit > 0 ? limit : 20;
    const agentPrefix = `${agentId}:`;
    const normalizeStatusId = (value) => {
      const id = typeof value === "string" ? value.trim() : "";
      return id.startsWith(agentPrefix) ? id.slice(agentPrefix.length) : id;
    };
    if (probe.ready || agentId === "claude-code") {
      try {
        sessions = await adapter.listSessions({ limit: safeLimit });
      } catch (error) {
        this.log("warn", "listSessions failed", { agentId, error: String(error?.message || error) });
      }
    }
    if (this.sessionStatusProvider) {
      try {
        const statuses = await this.sessionStatusProvider(agentId);
        const byId = new Map();
        for (const status of Array.isArray(statuses) ? statuses : []) {
          const rawId = typeof status?.id === "string" ? status.id.trim() : "";
          const id = normalizeStatusId(rawId);
          if (!id) continue;
          byId.set(id, status);
          if (rawId) byId.set(rawId, status);
        }
        const seen = new Set();
        sessions = sessions.map((session) => {
          const rawId = typeof session?.id === "string" ? session.id.trim() : "";
          const id = normalizeStatusId(rawId);
          if (id) seen.add(id);
          const status = byId.get(rawId) || byId.get(id);
          if (status) {
            const state = status.state || "idle";
            const displayContent = normalizeSessionDisplayContent(state, status.displayContent);
            return {
              ...session,
              state,
              statusUpdatedAt: status.updatedAt || 0,
              ...(typeof status.displayTitle === "string" && status.displayTitle
                ? { displayTitle: status.displayTitle }
                : {}),
              ...(displayContent
                ? { displayContent }
                : {}),
              ...(typeof status.transcriptPath === "string" && status.transcriptPath
                ? { transcriptPath: status.transcriptPath }
                : {}),
            };
          }
          return session;
        });
        if (agentId === "claude-code" || agentId === "codex" || agentId === "mimocode") {
          for (const status of Array.isArray(statuses) ? statuses : []) {
            const id = normalizeStatusId(status?.id);
            if (!id || seen.has(id)) continue;
            const state = status.state || "idle";
            const statusTitle = typeof status.title === "string" ? status.title.trim() : "";
            const displayTitle = typeof status.displayTitle === "string"
              ? status.displayTitle.trim()
              : "";
            const title = agentId === "codex"
              ? [statusTitle, displayTitle].find((value) => value && value !== "Codex 会话") || ""
              : statusTitle || displayTitle || (agentId === "mimocode" ? "MiMoCode 会话" : "Claude 会话");
            const resolvedDisplayTitle = agentId === "codex" && displayTitle === "Codex 会话"
              ? title
              : displayTitle || title;
            const displayContent = normalizeSessionDisplayContent(state, status.displayContent);
            // Codex's durable session scan can briefly omit a live Desktop
            // thread. Keep only titled active tracker records as the bridge;
            // unseen terminal records may be internal turns or old history.
            if (agentId === "codex" && (!ACTIVE_SESSION_STATES.has(state) || !title)) {
              continue;
            }
            if (
              agentId === "claude-code"
              && title === "Claude 会话"
              && !displayContent
            ) {
              continue;
            }
            sessions.push({
              id,
              name: title,
              summary: title,
              cwd: typeof status.cwd === "string" ? status.cwd : undefined,
              lastModified: status.updatedAt || 0,
              statusUpdatedAt: status.updatedAt || 0,
              state,
              displayTitle: resolvedDisplayTitle,
              displayContent,
              transcriptPath: status.transcriptPath || undefined,
              surface: "desktop",
            });
            seen.add(id);
          }
        }
      } catch (error) {
        this.log("warn", "session status provider failed", { agentId, error: String(error?.message || error) });
      }
    }
    if (agentId === "codex") {
      sessions = sessions.filter((session) => !isInternalCodexSession(session));
    }
    sessions.sort((a, b) => (b.statusUpdatedAt || b.lastModified || 0) - (a.statusUpdatedAt || a.lastModified || 0));
    if (
      agentId === "claude-code"
      && sessions.some((session) => normalizeStatusId(session?.id) !== "claude:local")
    ) {
      sessions = sessions.filter((session) => normalizeStatusId(session?.id) !== "claude:local");
    }
    // Claude CLI history is broader than the Desktop client's routable Code
    // list. Once Desktop metadata or a Desktop hook identifies that surface,
    // keep the device queue on that surface so previous/next never selects a
    // JSONL that claude://resume cannot open.
    if (
      agentId === "claude-code"
      && sessions.some((session) => session?.surface === "desktop")
    ) {
      sessions = sessions.filter((session) => session?.surface === "desktop");
    }
    sessions = sessions.slice(0, safeLimit);
    const ready = probe.ready || sessions.length > 0;
    sendJson(req, res, 200, {
      ok: true,
      agentId,
      ready,
      reason: ready ? null : probe.reason,
      sessions,
    });
  }

  async _handleSessionEvents(req, res, url) {
    const agentId = url.searchParams.get("agentId");
    if (!agentId) {
      sendJson(req, res, 400, { ok: false, error: "agentId is required" });
      return;
    }
    if (!this.registry.get(agentId)) {
      sendJson(req, res, 404, { ok: false, error: `unknown agentId: ${agentId}` });
      return;
    }

    const hasCursor = url.searchParams.has("cursor");
    const cursorText = url.searchParams.get("cursor");
    const cursor = hasCursor ? Number(cursorText) : undefined;
    if (hasCursor && (!Number.isFinite(cursor) || cursor < 0)) {
      sendJson(req, res, 400, { ok: false, error: "cursor must be a non-negative number" });
      return;
    }
    const requestedLimit = Number.parseInt(url.searchParams.get("limit") || "100", 10);
    const limit = Number.isFinite(requestedLimit) && requestedLimit > 0
      ? Math.min(requestedLimit, 200)
      : 100;
    const streamId = url.searchParams.get("streamId") || "";
    let result = { streamId: "", cursor: 0, reset: false, events: [] };
    if (this.sessionEventProvider) {
      result = await this.sessionEventProvider(agentId, {
        cursor,
        streamId,
        limit,
      }) || result;
    }

    const agentPrefix = `${agentId}:`;
    const normalizeStatusId = (value) => {
      const id = typeof value === "string" ? value.trim() : "";
      return id.startsWith(agentPrefix) ? id.slice(agentPrefix.length) : id;
    };
    const events = (Array.isArray(result.events) ? result.events : [])
      .map((event) => {
        const session = event?.session && typeof event.session === "object"
          ? event.session
          : null;
        const id = normalizeStatusId(session?.id);
        if (!session || !id) return null;
        return {
          ...event,
          session: { ...session, id },
        };
      })
      .filter((event) => Boolean(event)
        && !(agentId === "codex" && isInternalCodexSession(event.session)));
    sendJson(req, res, 200, {
      ok: true,
      agentId,
      streamId: typeof result.streamId === "string" ? result.streamId : "",
      cursor: Number.isFinite(Number(result.cursor)) ? Number(result.cursor) : 0,
      reset: result.reset === true,
      events,
    });
  }

  async _handleInject(req, res, _url) {
    let body;
    try {
      body = await readJsonBody(req);
    } catch (error) {
      sendJson(req, res, error?.statusCode || 400, { ok: false, error: error?.message || "bad request", code: error?.code || "BAD_REQUEST" });
      return;
    }

    const agentId = typeof body.agentId === "string" ? body.agentId.trim() : "";
    const text = typeof body.text === "string" ? body.text : "";
    const sessionIdRaw = typeof body.sessionId === "string" && body.sessionId.trim() !== "" ? body.sessionId.trim() : "auto";
    const metadata = body.metadata && typeof body.metadata === "object" ? body.metadata : {};

    if (!agentId) {
      sendJson(req, res, 400, { ok: false, error: "agentId is required" });
      return;
    }
    if (!text || !text.trim()) {
      sendJson(req, res, 400, { ok: false, error: "text must be a non-empty string" });
      return;
    }

    const adapter = this.registry.get(agentId);
    if (!adapter) {
      sendJson(req, res, 404, { ok: false, error: `unknown agentId: ${agentId}` });
      return;
    }

    const probe = await this.registry.statusOne(agentId, { fresh: true });
    if (!probe.ready) {
      sendJson(req, res, 503, { ok: false, error: probe.reason || "agent unavailable", code: "AGENT_UNAVAILABLE" });
      return;
    }

    const runId = crypto.randomUUID();
    const ac = new AbortController();
    let resolvedSessionId = sessionIdRaw;
    let opened = false;
    // The cwd that *belongs to* the resolved session, when the adapter can
    // tell us. For Claude Code this is critical: claude indexes session
    // jsonl per-cwd at `~/.claude/projects/<encoded-cwd>/<sid>.jsonl`, so
    // running `claude --resume <sid>` from the wrong cwd makes the CLI
    // exit instantly with `No conversation found with session ID: <sid>`
    // — which used to surface as a SSE that opened, fired `ready`, and
    // then `done` ~250 ms later with no token frames at all (because
    // stream-json mapped the error result frame to `done`). Honor the
    // session's own cwd whenever the request didn't pin one explicitly.
    let resolvedSessionCwd =
      typeof metadata.cwd === "string" && metadata.cwd ? metadata.cwd : "";

    if (sessionIdRaw === "auto" || sessionIdRaw === "new") {
      // sessionId="new" is the BusLLM voice-session sentinel: skip
      // resolveActive entirely (the user just toggled voice on, or
      // barge-in caused us to invalidate the previous sid) and mint
      // a brand-new agent session. sessionId="auto" keeps the legacy
      // resolve-or-open behaviour for any non-voice caller (REPL,
      // direct curl, future agent UI).
      try {
        let active = null;
        if (sessionIdRaw === "auto") {
          active = await adapter.resolveActive();
        }
        if (active && active.id) {
          resolvedSessionId = active.id;
          if (!resolvedSessionCwd && typeof active.cwd === "string" && active.cwd) {
            resolvedSessionCwd = active.cwd;
          }
        } else {
          const fresh = await adapter.openNew({
            cwd: resolvedSessionCwd || undefined,
          });
          resolvedSessionId = fresh.id || "";
          if (!resolvedSessionCwd && typeof fresh.cwd === "string" && fresh.cwd) {
            resolvedSessionCwd = fresh.cwd;
          }
          opened = true;
        }
      } catch (error) {
        sendJson(req, res, 500, {
          ok: false,
          error: `failed to resolve session: ${error?.message || error}`,
          code: "SESSION_RESOLVE_FAILED",
        });
        return;
      }
    } else if (!resolvedSessionCwd) {
      // Caller pinned a specific session id but didn't tell us its cwd —
      // walk listSessions to look it up.
      try {
        const all = await adapter.listSessions({ limit: 200 });
        const match = Array.isArray(all)
          ? all.find((s) => s && s.id === resolvedSessionId)
          : null;
        if (match && typeof match.cwd === "string" && match.cwd) {
          resolvedSessionCwd = match.cwd;
        }
      } catch {
        /* not fatal — adapter falls back to its default cwd */
      }
    }

    openSseStream(res, corsHeaders(req));
    writeSseEvent(res, "ready", { runId, agentId, sessionId: resolvedSessionId, opened });
    const stopHeartbeat = startSseHeartbeat(res);
    this._activeRuns.set(runId, {
      abort: () => ac.abort(),
      agentId,
      sessionId: resolvedSessionId,
      startedAt: Date.now(),
    });

    const cleanup = () => {
      stopHeartbeat();
      this._activeRuns.delete(runId);
    };

    res.once("close", () => {
      ac.abort();
      cleanup();
    });

    let terminalSent = false;
    try {
      const stream = adapter.inject({
        sessionId: resolvedSessionId || "auto",
        text: text.trim(),
        metadata,
        signal: ac.signal,
        cwd: resolvedSessionCwd || undefined,
      });

      for await (const evt of stream) {
        if (!evt || typeof evt !== "object" || typeof evt.kind !== "string") continue;
        if (terminalSent) continue;
        switch (evt.kind) {
          case "token":
            writeSseEvent(res, "token", { text: typeof evt.text === "string" ? evt.text : "" });
            break;
          case "tool":
            writeSseEvent(res, "tool", {
              name: typeof evt.name === "string" ? evt.name : "unknown",
              phase: evt.phase === "end" ? "end" : "start",
              input: evt.input,
              ok: evt.ok,
            });
            break;
          case "done":
            writeSseEvent(res, "done", {
              sessionId: typeof evt.sessionId === "string" && evt.sessionId ? evt.sessionId : resolvedSessionId,
              tokens: Number.isFinite(evt.tokens) ? evt.tokens : undefined,
              stopReason: typeof evt.stopReason === "string" ? evt.stopReason : undefined,
            });
            terminalSent = true;
            break;
          case "error":
            writeSseEvent(res, "error", {
              code: typeof evt.code === "string" ? evt.code : "ADAPTER_ERROR",
              message: typeof evt.message === "string" ? evt.message : "adapter error",
            });
            terminalSent = true;
            break;
          default:
            this.log("warn", "unknown adapter event kind", { kind: evt.kind, agentId });
        }
      }

      if (!terminalSent) {
        writeSseEvent(res, "done", { sessionId: resolvedSessionId, stopReason: "stream_ended" });
      }
    } catch (error) {
      const message = error?.message || String(error);
      this.log("error", "inject stream failed", { agentId, runId, error: message });
      if (!terminalSent) {
        writeSseEvent(res, "error", {
          code: ac.signal.aborted ? "CANCELLED" : "STREAM_INTERRUPTED",
          message,
        });
      }
    } finally {
      cleanup();
      endSseStream(res);
    }
  }

  async _handleCancel(req, res, _url) {
    let body;
    try {
      body = await readJsonBody(req);
    } catch (error) {
      sendJson(req, res, error?.statusCode || 400, { ok: false, error: error?.message || "bad request" });
      return;
    }
    const runId = typeof body.runId === "string" ? body.runId.trim() : "";
    if (!runId) {
      sendJson(req, res, 400, { ok: false, error: "runId is required" });
      return;
    }
    const run = this._activeRuns.get(runId);
    if (!run) {
      sendJson(req, res, 404, { ok: false, error: `no active run ${runId}` });
      return;
    }
    try { run.abort(); } catch { /* ignore */ }
    this._activeRuns.delete(runId);
    sendJson(req, res, 200, { ok: true, runId, agentId: run.agentId });
  }
}

module.exports = {
  AgentSessionBus,
};
