"use strict";

const { spawn } = require("child_process");
const path = require("path");

const { BaseAdapter } = require("./base");
const { locateOpenClaw, listOpenClawSessions } = require("../util/openclaw-paths");

const HELPER_PATH = path.join(__dirname, "openclaw-helper.mjs");
const EVENT_PREFIX = "@@AGENT_BUS@@";

/**
 * OpenClaw adapter.
 *
 * **Why this is shaped differently from Claude/Codex.**
 * OpenClaw isn't driven over a pipe-style CLI. It's an npm package
 * (`openclaw`) whose `dist/plugin-sdk/agent-runtime.js` exports an in-process
 * `agentCommand()` function that pet-claw already drives via a helper-process
 * pattern. We mirror that pattern: spawn `node openclaw-helper.mjs`, pipe
 * the user message in via stdin, and parse `@@AGENT_BUS@@`-prefixed event
 * lines from stdout.
 *
 * **Machine-agnostic guarantees** (same contract as Claude/Codex adapters):
 * - `isAvailable()` resolves the `openclaw` package across npm/pnpm/yarn/nvm/
 *   homebrew/known-paths and reports `not_installed` cleanly when missing.
 *   Does not assume the user has openclaw installed.
 * - `listSessions()` reads `~/.openclaw/agents/<agentId>/sessions/sessions.json`,
 *   gracefully returning [] when the user has never started a session.
 * - `inject()` always passes through the helper child so the parent process
 *   never imports openclaw directly (keeps the bus zero-deps).
 *
 * Configuration via env (all optional):
 *   OPENCLAW_HOME              package root override
 *   OPENCLAW_RUNTIME_MODULE    direct path to agent-runtime.js
 *   OPENCLAW_STATE_DIR         override for ~/.openclaw
 *   OPENCLAW_AGENT_ID          override default agent id (default "main")
 *   OPENCLAW_NODE_BIN          path to node binary used to spawn the helper
 */
class OpenClawAdapter extends BaseAdapter {
  /**
   * @param {object} [opts]
   * @param {(level:string,msg:string,details?:object)=>void} [opts.log]
   * @param {NodeJS.ProcessEnv} [opts.env]
   * @param {string} [opts.cwd]
   * @param {string} [opts.helperPath]   override the helper script (testing seam)
   * @param {string} [opts.nodeBin]      override the node binary used (default: process.execPath)
   */
  constructor({ log, env, cwd, helperPath, nodeBin } = {}) {
    super({ agentId: "openclaw", log });
    this._env = env || process.env;
    this._cwd = cwd || process.cwd();
    this._helperPath = helperPath || HELPER_PATH;
    this._nodeBin = nodeBin
      || this._env.OPENCLAW_NODE_BIN
      || process.execPath;
    this._cachedAvailability = null;
    this._cachedAt = 0;
  }

  _agentId() {
    return (this._env.OPENCLAW_AGENT_ID && this._env.OPENCLAW_AGENT_ID.trim()) || "main";
  }

  async isAvailable() {
    const now = Date.now();
    if (this._cachedAvailability && now - this._cachedAt < 5000) {
      return this._cachedAvailability;
    }

    let value;
    const found = locateOpenClaw({ env: this._env });
    if (!found) {
      value = {
        ready: false,
        reason: "openclaw 未安装（请运行 `npm i -g openclaw` 或设置 OPENCLAW_HOME）",
      };
    } else {
      value = {
        ready: true,
        reason: null,
        details: {
          packageRoot: found.packageRoot,
          packageVersion: found.packageVersion,
        },
      };
    }
    this._cachedAvailability = value;
    this._cachedAt = now;
    return value;
  }

  async listSessions({ limit = 20 } = {}) {
    const sessions = listOpenClawSessions({ agentId: this._agentId(), env: this._env });
    return sessions.slice(0, limit);
  }

  async openNew(/* opts */) {
    return { id: "", lastModified: Date.now(), summary: "new (openclaw)" };
  }

  /**
   * @param {import("./base").InjectRequest} req
   */
  async *inject(req) {
    const { text, signal } = req;
    const sessionId = req.sessionId && req.sessionId !== "auto" ? req.sessionId : "";
    const cwd = typeof req.cwd === "string" && req.cwd ? req.cwd : this._cwd;

    const probe = await this.isAvailable();
    if (!probe.ready) {
      yield { kind: "error", code: "AGENT_UNAVAILABLE", message: probe.reason || "openclaw unavailable" };
      return;
    }

    const found = locateOpenClaw({ env: this._env });
    if (!found) {
      yield { kind: "error", code: "AGENT_UNAVAILABLE", message: "openclaw 未安装" };
      return;
    }

    // metadata.extraSystemPrompt: voice-mode hints (and other per-turn
    // instructions) ride here. OpenClaw's `agentCommand` natively
    // accepts an `extraSystemPrompt` argument that splices into the
    // system message — we forward it directly so we don't have to
    // contaminate the user turn the way Claude/Codex adapters must.
    const extraSystemPrompt = pickExtraSystemPrompt(req.metadata);

    yield* this._spawnAndIterate({
      runtimeModule: found.runtimeModule,
      cwd,
      signal,
      input: {
        message: text,
        agentId: this._agentId(),
        sessionId: sessionId || undefined,
        runId: req.requestId || undefined,
        ...(extraSystemPrompt ? { extraSystemPrompt } : {}),
      },
    });
  }

  async *_spawnAndIterate({ runtimeModule, cwd, signal, input }) {
    this.log("info", "spawning openclaw helper", {
      helper: this._helperPath,
      runtimeModule,
      cwd,
      sessionId: input.sessionId || null,
    });

    const child = spawn(this._nodeBin, [this._helperPath], {
      cwd,
      env: {
        ...this._env,
        OPENCLAW_RUNTIME_MODULE_RESOLVED: runtimeModule,
      },
      stdio: ["pipe", "pipe", "pipe"],
    });

    const aborter = signal && !signal.aborted
      ? () => { try { child.kill("SIGTERM"); } catch { /* ignore */ } }
      : null;
    if (aborter && signal) signal.addEventListener("abort", aborter, { once: true });
    if (signal && signal.aborted) {
      try { child.kill("SIGTERM"); } catch { /* ignore */ }
    }

    let stderrBuf = "";
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk) => {
      stderrBuf += chunk;
      if (stderrBuf.length > 8192) stderrBuf = stderrBuf.slice(-8192);
    });

    try { child.stdin.end(JSON.stringify(input)); }
    catch { /* child died early; close handler will surface it */ }

    let resolvedSessionId = input.sessionId || "";
    let sawTerminal = false;

    const exitPromise = new Promise((resolveExit) => {
      child.once("close", (code, sig) => resolveExit({ code, sig }));
      child.once("error", (err) => resolveExit({ code: -1, sig: null, err }));
    });

    try {
      child.stdout.setEncoding("utf8");
      let buffer = "";
      for await (const chunk of child.stdout) {
        buffer += chunk;
        let idx;
        while ((idx = buffer.indexOf("\n")) >= 0) {
          const line = buffer.slice(0, idx);
          buffer = buffer.slice(idx + 1);
          if (!line) continue;
          for (const evt of this._consumeHelperLine(line, (sid) => { resolvedSessionId = sid; })) {
            if (evt.kind === "done" || evt.kind === "error") sawTerminal = true;
            yield evt;
          }
        }
      }
      if (buffer.trim()) {
        for (const evt of this._consumeHelperLine(buffer, (sid) => { resolvedSessionId = sid; })) {
          if (evt.kind === "done" || evt.kind === "error") sawTerminal = true;
          yield evt;
        }
      }

      const { code, sig, err } = await exitPromise;
      if (!sawTerminal) {
        if (err) {
          yield { kind: "error", code: "SPAWN_FAILED", message: String(err.message || err) };
        } else if (code !== 0) {
          yield {
            kind: "error",
            code: "AGENT_EXIT_NONZERO",
            message: `openclaw helper exited code=${code} sig=${sig || ""}: ${stderrBuf.trim().slice(0, 512)}`,
          };
        } else {
          yield { kind: "done", sessionId: resolvedSessionId, stopReason: "stream_ended" };
        }
      }
    } finally {
      if (aborter && signal) signal.removeEventListener("abort", aborter);
      if (!child.killed) {
        try { child.kill("SIGTERM"); } catch { /* ignore */ }
      }
    }
  }

  *_consumeHelperLine(line, onSessionId) {
    const trimmed = line.trim();
    if (!trimmed) return;
    if (!trimmed.startsWith(EVENT_PREFIX)) return;

    let record;
    try {
      record = JSON.parse(trimmed.slice(EVENT_PREFIX.length));
    } catch {
      return;
    }
    if (!record || typeof record !== "object") return;

    if (record.type === "session" && typeof record.id === "string" && record.id) {
      onSessionId(record.id);
      return;
    }
    if (record.type === "event" && record.event) {
      yield record.event;
      return;
    }
    if (record.type === "result") {
      if (typeof record.sessionId === "string" && record.sessionId) {
        onSessionId(record.sessionId);
      }
      // Some openclaw setups emit no per-token deltas (because pi-embedded
      // event stream isn't loaded). Surface the final payload as one token
      // so the bus still has something to TTS.
      if (typeof record.text === "string" && record.text) {
        yield { kind: "token", text: record.text };
      }
      yield {
        kind: "done",
        sessionId: typeof record.sessionId === "string" ? record.sessionId : "",
        stopReason: "end_turn",
      };
      return;
    }
    if (record.type === "error") {
      yield {
        kind: "error",
        code: record.error?.name || "AGENT_ERROR",
        message: record.error?.message || "openclaw helper failed",
      };
    }
  }
}

function pickExtraSystemPrompt(metadata) {
  if (!metadata || typeof metadata !== "object") return "";
  const v = metadata.extraSystemPrompt;
  return typeof v === "string" && v.trim() ? v.trim() : "";
}

module.exports = {
  OpenClawAdapter,
  EVENT_PREFIX,
};
