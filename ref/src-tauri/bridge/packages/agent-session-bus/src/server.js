"use strict";

const crypto = require("crypto");
const http = require("http");

const {
  openSseStream,
  writeSseEvent,
  endSseStream,
  startSseHeartbeat,
} = require("./sse");

const MAX_REQUEST_BYTES = 64 * 1024;

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

// CORS for the bus. The bus binds 127.0.0.1 only, so we don't have a real
// cross-origin trust problem, but the Tauri webview runs at
// `tauri://localhost` and WebKit blocks loopback fetches without explicit
// CORS headers (every adapter status poll was 200 OK on the wire but the
// renderer dropped the body — invisible from the user's POV beyond a stuck
// "正在检测..." banner). We mirror the request's Origin instead of using
// `*` so this composes with future auth setups that need
// `Access-Control-Allow-Credentials: true`.
const ALLOWED_REQUEST_HEADERS = "content-type, accept, x-requested-with";
const ALLOWED_METHODS = "GET, POST, OPTIONS";
function corsHeaders(req) {
  const origin = req.headers.origin || "*";
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
   * @param {(level: string, msg: string, details?: object) => void} [opts.log]
   */
  constructor({ port = 8181, host = "127.0.0.1", registry, log } = {}) {
    if (!registry) throw new Error("AgentSessionBus requires a registry");
    this.port = port;
    this.host = host;
    this.registry = registry;
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
    if (!probe.ready) {
      sendJson(req, res, 200, { ok: true, agentId, ready: false, reason: probe.reason, sessions: [] });
      return;
    }
    let sessions = [];
    try {
      sessions = await adapter.listSessions({ limit: Number.isFinite(limit) && limit > 0 ? limit : 20 });
    } catch (error) {
      this.log("warn", "listSessions failed", { agentId, error: String(error?.message || error) });
    }
    sendJson(req, res, 200, { ok: true, agentId, ready: true, sessions });
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
            cleanup();
            endSseStream(res);
            return;
          case "error":
            writeSseEvent(res, "error", {
              code: typeof evt.code === "string" ? evt.code : "ADAPTER_ERROR",
              message: typeof evt.message === "string" ? evt.message : "adapter error",
            });
            cleanup();
            endSseStream(res);
            return;
          default:
            this.log("warn", "unknown adapter event kind", { kind: evt.kind, agentId });
        }
      }

      writeSseEvent(res, "done", { sessionId: resolvedSessionId, stopReason: "stream_ended" });
    } catch (error) {
      const message = error?.message || String(error);
      this.log("error", "inject stream failed", { agentId, runId, error: message });
      writeSseEvent(res, "error", {
        code: ac.signal.aborted ? "CANCELLED" : "STREAM_INTERRUPTED",
        message,
      });
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
