"use strict";

const { spawn } = require("child_process");
const path = require("path");

const { BaseAdapter } = require("./base");
const {
  findExecutable,
  readVersion,
  compareVersions,
} = require("../util/binary-resolver");
const { listClaudeSessions } = require("../util/claude-paths");
const { parseLine, frameToEvents } = require("../util/stream-json");

/**
 * Claude Code adapter — drives `claude -p ... --resume <sid>
 * --output-format stream-json --verbose` and translates the stream-json
 * output into AgentEvents.
 *
 * Designed to be machine-agnostic. It must not assume the user has Claude
 * Code installed; `isAvailable()` is the source of truth and the bus / UI
 * use that to decide whether to enable voice for this agent.
 *
 * The fallback paths here are kept in lock-step with
 * `lib.rs::detect_claude_code` (Rust side). If you add a new path on one
 * side, add it on the other in the same PR — otherwise the user will see
 * "已安装" in the agent picker but voice will say "claude not found", or
 * vice versa.
 */
class ClaudeCodeAdapter extends BaseAdapter {
  /**
   * @param {object} [opts]
   * @param {string} [opts.minVersion]   minimum CLI version we trust to speak stream-json. Default "1.0.0".
   * @param {(level:string,msg:string,details?:object)=>void} [opts.log]
   * @param {NodeJS.ProcessEnv} [opts.env]
   * @param {string} [opts.cwd]          default cwd for spawned `claude` (only used when caller doesn't pass one)
   * @param {string[]} [opts.fallbackPaths]   override fallback bin paths (testing seam)
   * @param {string[]} [opts.extraPathDirs]   override PATH dirs to merge in (testing seam)
   */
  constructor({
    minVersion = "1.0.0",
    log,
    env,
    cwd,
    fallbackPaths,
    extraPathDirs,
  } = {}) {
    super({ agentId: "claude-code", log });
    this._minVersion = minVersion;
    this._env = env || process.env;
    this._cwd = cwd || process.cwd();
    this._fallbackPathsOverride = Array.isArray(fallbackPaths) ? fallbackPaths : null;
    this._extraPathDirsOverride = Array.isArray(extraPathDirs) ? extraPathDirs : null;
    /** @type {string | null} */
    this._cachedBin = null;
    this._cachedAvailability = null;
    this._cachedAt = 0;
  }

  _defaultFallbackPaths() {
    const home = this._env.HOME || this._env.USERPROFILE || "~";
    // keep this list in lock-step with lib.rs::detect_claude_code
    return [
      `${home}/.local/bin/claude`,
      `${home}/.claude/local/claude`,
      "/opt/homebrew/bin/claude",
      "/usr/local/bin/claude",
      ...windowsCliFiles(this._env, "claude"),
    ];
  }

  _defaultExtraPathDirs() {
    const home = this._env.HOME || this._env.USERPROFILE || "~";
    // Tauri-launched processes inherit a stripped PATH on macOS — augment.
    return [
      `${home}/.npm-global/bin`,
      `${home}/.local/bin`,
      `${home}/.claude/local`,
      "/opt/homebrew/bin",
      "/usr/local/bin",
      ...windowsCliDirs(this._env),
    ];
  }

  _resolveBinary() {
    if (this._cachedBin) return this._cachedBin;
    const found = findExecutable({
      binName: "claude",
      envVar: "CLAUDE_CLI_PATH",
      env: this._env,
      fallbackPaths: this._fallbackPathsOverride !== null
        ? this._fallbackPathsOverride
        : this._defaultFallbackPaths(),
      extraPathDirs: this._extraPathDirsOverride !== null
        ? this._extraPathDirsOverride
        : this._defaultExtraPathDirs(),
    });
    this._cachedBin = found;
    return found;
  }

  async isAvailable() {
    const now = Date.now();
    if (this._cachedAvailability && now - this._cachedAt < 5000) {
      return this._cachedAvailability;
    }
    let value;
    const bin = this._resolveBinary();
    if (!bin) {
      value = {
        ready: false,
        reason: "claude CLI 未找到（请运行 `npm i -g @anthropic-ai/claude-code` 或设置 CLAUDE_CLI_PATH）",
      };
    } else {
      const version = readVersion(bin, { env: this._env });
      if (!version) {
        value = { ready: false, reason: `claude --version 调用失败 (${bin})` };
      } else if (compareVersions(version, this._minVersion) < 0) {
        value = {
          ready: false,
          reason: `claude 版本 ${version} 低于最低支持 ${this._minVersion}`,
        };
      } else {
        value = { ready: true, reason: null };
      }
    }
    this._cachedAvailability = value;
    this._cachedAt = now;
    return value;
  }

  async listSessions({ limit = 20 } = {}) {
    return listClaudeSessions({ limit, env: this._env });
  }

  async openNew(/* opts */) {
    // Claude mints the session id on the first injected message — we can't
    // pre-allocate one. Return a placeholder; inject() will fill in the id
    // from the first frame that carries it (see _injectImpl).
    return { id: "", lastModified: Date.now(), summary: "new (claude)" };
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
      yield { kind: "error", code: "AGENT_UNAVAILABLE", message: probe.reason || "claude unavailable" };
      return;
    }

    const bin = this._resolveBinary();
    if (!bin) {
      yield { kind: "error", code: "AGENT_UNAVAILABLE", message: "claude CLI 未找到" };
      return;
    }

    // metadata.extraSystemPrompt: voice-mode hints (and other per-turn
    // instructions) ride here. Claude Code natively exposes
    // `--append-system-prompt`, so we use it instead of polluting the
    // user turn — that way the hint is invisible to the assistant
    // transcript Claude stores on disk and to any tools that scan
    // session history for past *user* messages.
    const hint = pickExtraSystemPrompt(req.metadata);

    const args = ["-p", text, "--output-format", "stream-json", "--verbose"];
    if (hint) args.push("--append-system-prompt", hint);
    if (sessionId) args.splice(2, 0, "--resume", sessionId);

    yield* this._spawnAndIterate({ bin, args, cwd, signal, fallbackSessionId: sessionId });
  }

  async *_spawnAndIterate({ bin, args, cwd, signal, fallbackSessionId }) {
    this.log("info", "spawning claude", { bin, args, cwd, hasSession: Boolean(fallbackSessionId) });
    const useWindowsCmdShell = process.platform === "win32" && /\.(cmd|bat)$/i.test(bin);
    const child = spawn(bin, args, {
      cwd,
      env: this._spawnEnv(),
      stdio: ["ignore", "pipe", "pipe"],
      shell: useWindowsCmdShell,
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

    let resolvedSessionId = fallbackSessionId || "";
    let sawDone = false;

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

          const frame = parseLine(line);
          if (!frame) continue;

          // Capture session id as soon as Claude tells us, even from frames
          // that don't otherwise emit a public event (e.g. system/init).
          const sid = pickSessionId(frame);
          if (sid && !resolvedSessionId) resolvedSessionId = sid;

          for (const evt of frameToEvents(frame)) {
            if (evt.kind === "done") {
              sawDone = true;
              yield {
                ...evt,
                sessionId: evt.sessionId || resolvedSessionId,
              };
            } else {
              yield evt;
            }
          }
        }
      }
      if (buffer.trim()) {
        const frame = parseLine(buffer);
        if (frame) {
          for (const evt of frameToEvents(frame)) {
            if (evt.kind === "done") sawDone = true;
            yield evt;
          }
        }
      }

      const { code, sig, err } = await exitPromise;
      if (!sawDone) {
        if (err) {
          yield { kind: "error", code: "SPAWN_FAILED", message: String(err.message || err) };
        } else if (code !== 0) {
          yield {
            kind: "error",
            code: "AGENT_EXIT_NONZERO",
            message: `claude exited with code=${code} sig=${sig || ""}: ${stderrBuf.trim().slice(0, 512)}`,
          };
        } else {
          yield {
            kind: "done",
            sessionId: resolvedSessionId,
            stopReason: "stream_ended",
          };
        }
      }
    } finally {
      if (aborter && signal) signal.removeEventListener("abort", aborter);
      if (!child.killed) {
        try { child.kill("SIGTERM"); } catch { /* ignore */ }
      }
    }
  }

  _spawnEnv() {
    const home = this._env.HOME || this._env.USERPROFILE || "";
    // Re-derive PATH the same way the resolver does so the spawned process
    // can find peer tools (node, git, ripgrep, ...) that Claude may invoke.
    const sep = process.platform === "win32" ? ";" : ":";
    const extras = [
      `${home}/.npm-global/bin`,
      `${home}/.local/bin`,
      `${home}/.claude/local`,
      "/opt/homebrew/bin",
      "/usr/local/bin",
      ...windowsCliDirs(this._env),
    ];
    const current = (this._env.PATH || "").split(sep).filter(Boolean);
    const merged = [...new Set([...extras, ...current])].join(sep);
    return { ...this._env, PATH: merged };
  }
}

function windowsCliDirs(env) {
  const dirs = [];
  const push = (value) => {
    if (value && !dirs.includes(value)) dirs.push(value);
  };
  if (env.APPDATA) push(`${env.APPDATA}\\npm`);
  if (env.LOCALAPPDATA) {
    push(`${env.LOCALAPPDATA}\\Microsoft\\WindowsApps`);
    push(`${env.LOCALAPPDATA}\\pnpm`);
  }
  if (env.USERPROFILE) {
    push(`${env.USERPROFILE}\\scoop\\shims`);
    push(`${env.USERPROFILE}\\.local\\bin`);
  }
  if (env.ProgramFiles) push(`${env.ProgramFiles}\\nodejs`);
  if (env["ProgramFiles(x86)"]) push(`${env["ProgramFiles(x86)"]}\\nodejs`);
  return dirs;
}

function windowsCliFiles(env, name) {
  const out = [];
  for (const dir of windowsCliDirs(env)) {
    out.push(`${dir}\\${name}.exe`, `${dir}\\${name}.cmd`, `${dir}\\${name}.bat`);
  }
  return out;
}

function pickSessionId(frame) {
  if (!frame || typeof frame !== "object") return "";
  if (typeof frame.session_id === "string") return frame.session_id;
  if (typeof frame.sessionId === "string") return frame.sessionId;
  return "";
}

function pickExtraSystemPrompt(metadata) {
  if (!metadata || typeof metadata !== "object") return "";
  const v = metadata.extraSystemPrompt;
  return typeof v === "string" && v.trim() ? v.trim() : "";
}

module.exports = {
  ClaudeCodeAdapter,
};
