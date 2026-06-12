"use strict";

const fs = require("fs");
const { spawn } = require("child_process");

const { BaseAdapter } = require("./base");
const {
  findExecutable,
  readVersion,
  compareVersions,
} = require("../util/binary-resolver");
const { listCodexSessions } = require("../util/codex-paths");
const { parseLine, frameToEvents } = require("../util/stream-json");

/**
 * Codex (OpenAI Codex CLI/Desktop) adapter. New sessions still use
 * `codex exec --json`, while existing Desktop threads are continued through
 * `codex app-server --listen stdio://` so the thread keeps its Desktop-owned
 * model/account configuration instead of being reinterpreted by `exec resume`.
 *
 * Like ClaudeCodeAdapter this is intentionally machine-agnostic. Discovery
 * fallback paths are kept in lock-step with `lib.rs::detect_codex` so the
 * HachimoDock UI's "Codex 已安装" badge agrees with the bus's notion of
 * availability.
 */
class CodexAdapter extends BaseAdapter {
  /**
   * @param {object} [opts]
   * @param {string} [opts.minVersion]   minimum CLI version. Default
   *   "0.118.0" — the first Codex Desktop release that exposes the
   *   line-delimited JSON shape we depend on (`exec --json`). Older builds
   *   had `exec --output-format stream-json`
   *   with a different schema; we don't try to support them.
   * @param {(level:string,msg:string,details?:object)=>void} [opts.log]
   * @param {NodeJS.ProcessEnv} [opts.env]
   * @param {string} [opts.cwd]
   * @param {string[]} [opts.fallbackPaths]
   * @param {string[]} [opts.extraPathDirs]
   * @param {string} [opts.sandbox]      Codex `--sandbox` policy. Default
   *   `workspace-write` matches Codex Desktop's interactive default.
   *   Voice users typically don't authenticate elevated changes, so
   *   leaving it permissive is safer than the alternative of failing
   *   silently when the agent tries to edit a file.
   */
  constructor({
    minVersion = "0.118.0",
    log,
    env,
    cwd,
    fallbackPaths,
    extraPathDirs,
    sandbox = "workspace-write",
  } = {}) {
    super({ agentId: "codex", log });
    this._minVersion = minVersion;
    this._env = env || process.env;
    this._cwd = cwd || process.cwd();
    this._fallbackPathsOverride = Array.isArray(fallbackPaths) ? fallbackPaths : null;
    this._extraPathDirsOverride = Array.isArray(extraPathDirs) ? extraPathDirs : null;
    this._sandbox = typeof sandbox === "string" && sandbox ? sandbox : "workspace-write";
    this._cachedBin = null;
    this._cachedAvailability = null;
    this._cachedAt = 0;
  }

  _defaultFallbackPaths() {
    const home = this._env.HOME || this._env.USERPROFILE || "~";
    // keep this list in lock-step with lib.rs::detect_codex
    return [
      `${home}/.local/bin/codex`,
      "/opt/homebrew/bin/codex",
      "/usr/local/bin/codex",
      ...windowsCliFiles(this._env, "codex"),
    ];
  }

  _defaultExtraPathDirs() {
    const home = this._env.HOME || this._env.USERPROFILE || "~";
    return [
      `${home}/.npm-global/bin`,
      `${home}/.local/bin`,
      "/opt/homebrew/bin",
      "/usr/local/bin",
      ...windowsCliDirs(this._env),
    ];
  }

  _resolveBinary() {
    if (this._cachedBin) return this._cachedBin;
    const found = findExecutable({
      binName: "codex",
      envVar: "CODEX_CLI_PATH",
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
        reason: "codex CLI 未找到（请运行 `npm i -g @openai/codex` 或设置 CODEX_CLI_PATH）",
      };
    } else {
      const version = readVersion(bin, { env: this._env });
      if (!version) {
        value = { ready: false, reason: `codex --version 调用失败 (${bin})` };
      } else if (compareVersions(version, this._minVersion) < 0) {
        value = {
          ready: false,
          reason: `codex 版本 ${version} 低于最低支持 ${this._minVersion}（需要 exec --json 支持）`,
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
    return listCodexSessions({ limit, env: this._env });
  }

  async resolveActive() {
    const sessions = await this.listSessions({ limit: 100 });
    if (sessions.length === 0) return null;

    const skipped = [];
    for (const session of sessions) {
      if (session && session.modelSupport === "unsupported") {
        skipped.push({ id: session.id, model: session.model || "" });
        continue;
      }

      if (skipped.length > 0) {
        this.log("warn", "codex auto skipped unsupported model sessions", {
          skipped,
          selectedSessionId: session?.id || "",
          selectedModel: session?.model || "",
        });
      }
      return session || null;
    }

    this.log("warn", "codex auto found only unsupported model sessions", { skipped });
    return null;
  }

  async openNew(/* opts */) {
    // Codex mints the sid on first turn — same as Claude. Caller treats
    // empty id as "auto" and the adapter fills it in from the first frame
    // that carries session_id.
    return { id: "", lastModified: Date.now(), summary: "new (codex)" };
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
      yield { kind: "error", code: "AGENT_UNAVAILABLE", message: probe.reason || "codex unavailable" };
      return;
    }
    const bin = this._resolveBinary();
    if (!bin) {
      yield { kind: "error", code: "AGENT_UNAVAILABLE", message: "codex CLI 未找到" };
      return;
    }

    // metadata.extraSystemPrompt: the bus folds voice-mode style hints (and
    // potentially other per-turn instructions) into the user turn. Codex
    // CLI doesn't expose a native append-system-prompt flag — we prepend
    // it inline. The hint is bracketed so the agent can recognise it as
    // out-of-band guidance rather than user content.
    const hint = pickExtraSystemPrompt(req.metadata);
    const finalText = hint ? `<voice_mode>\n${hint}\n</voice_mode>\n\n${text}` : text;

    if (sessionId && this._useAppServerResume()) {
      for await (const evt of this._spawnAppServerAndIterate({
        bin,
        cwd,
        signal,
        sessionId,
        text: finalText,
      })) {
        yield evt;
      }
      return;
    }

    // Codex CLI 0.118+ shape:
    //   New session: codex exec --json --skip-git-repo-check [--sandbox <policy>] -- "<text>"
    //   Resume:      codex exec resume --json --skip-git-repo-check <UUID> -- "<text>"
    //
    // Notes vs the old 0.40 stream-json shape:
    //   * `--output-format stream-json` no longer exists; `--json` does.
    //   * `--resume <sid>` flag was replaced by an `exec resume`
    //     subcommand that takes the UUID positionally. `exec resume
    //     --last` is also available but we don't use it (the bus
    //     picks the session).
    //   * Codex still requires non-`-` prompts to come last; clap
    //     accepts `--` as a flag terminator so we use it defensively.
    //   * `--skip-git-repo-check` lets the agent run outside a git
    //     repo (voice users may invoke us from a plain cwd). It's a
    //     no-op in a real repo.
    //   * `--sandbox <policy>` is **only valid on the new-session
    //     subcommand**, not on `exec resume`. Resume reuses the
    //     sandbox policy that was committed when the session was
    //     created. Passing `--sandbox` to `exec resume` makes Codex
    //     0.125 hard-error with `unexpected argument '--sandbox' found`,
    //     so we conditionally drop it for the resume path.
    const args = this._buildExecArgs({ sessionId, text: finalText });

    for await (const evt of this._spawnAndIterate({ bin, args, cwd, signal, fallbackSessionId: sessionId })) {
      if (
        evt
        && evt.kind === "error"
        && sessionId
        && isUnsupportedCodexModelError(evt.message)
      ) {
        this.log("warn", "codex resume model unsupported", {
          sessionId,
          error: evt.message,
        });
        yield {
          ...evt,
          code: "AGENT_UNSUPPORTED_MODEL",
          message: "Codex CLI resume 该会话时切到了当前账号不支持的模型；HachimoDock 没有传模型参数。请确认 Codex CLI 与 Codex Desktop 使用同一账号/模型能力，或改用 Codex Desktop 的续接通道。",
          details: evt.message,
        };
      } else {
        yield evt;
      }
    }
  }

  _buildExecArgs({ sessionId, text }) {
    const args = ["exec"];
    if (sessionId) args.push("resume");
    args.push("--json", "--skip-git-repo-check");
    if (!sessionId) args.push("--sandbox", this._sandbox);
    if (sessionId) args.push(sessionId);
    args.push("--", text);
    return args;
  }

  _useAppServerResume() {
    return this._env.CLAWD_CODEX_APP_SERVER !== "0";
  }

  async *_spawnAppServerAndIterate({ bin, cwd, signal, sessionId, text }) {
    this.log("info", "spawning codex app-server", { bin, cwd, sessionId });
    const useWindowsCmdShell = process.platform === "win32" && /\.(cmd|bat)$/i.test(bin);
    const child = spawn(bin, ["app-server", "--listen", "stdio://"], {
      cwd,
      env: this._spawnEnv(),
      stdio: ["pipe", "pipe", "pipe"],
      shell: useWindowsCmdShell,
    });

    const rpc = createStdioJsonRpcClient({
      child,
      log: this.log,
      stderrLimit: 8192,
    });
    const aborter = signal && !signal.aborted
      ? () => { try { child.kill("SIGTERM"); } catch { /* ignore */ } }
      : null;
    if (aborter && signal) signal.addEventListener("abort", aborter, { once: true });
    if (signal && signal.aborted) {
      try { child.kill("SIGTERM"); } catch { /* ignore */ }
    }

    let sawDone = false;
    let emittedText = "";
    const deltaItemIds = new Set();
    const emittedItemIds = new Set();
    let finalTurn = null;

    try {
      await rpc.request("initialize", {
        clientInfo: { name: "pet-manager", version: "0.1.0" },
        capabilities: { experimentalApi: true },
      });
      const resumeResult = await rpc.request("thread/resume", { threadId: sessionId });
      const resumedThreadId = pickThreadIdFromAppServerResult(resumeResult) || sessionId;
      await rpc.request("turn/start", {
        threadId: resumedThreadId,
        cwd,
        input: [{ type: "text", text }],
      });

      while (true) {
        const frame = await rpc.nextNotificationOrExit();
        if (!frame) break;
        const method = typeof frame.method === "string" ? frame.method : "";
        const params = frame.params && typeof frame.params === "object" ? frame.params : {};

        if (method === "item/agentMessage/delta" && typeof params.delta === "string" && params.delta) {
          if (typeof params.itemId === "string" && params.itemId) deltaItemIds.add(params.itemId);
          emittedText += params.delta;
          yield { kind: "token", text: params.delta };
          continue;
        }

        if (method === "item/completed") {
          const item = params.item && typeof params.item === "object" ? params.item : null;
          const textChunk = pickThreadItemText(item);
          const itemId = typeof item?.id === "string" ? item.id : "";
          if (
            textChunk
            && !(itemId && (deltaItemIds.has(itemId) || emittedItemIds.has(itemId)))
          ) {
            emittedText += textChunk;
            if (itemId) emittedItemIds.add(itemId);
            yield { kind: "token", text: textChunk };
          }
          for (const evt of appServerItemToToolEvents(item)) yield evt;
          continue;
        }

        if (method === "rawResponseItem/completed") {
          const textChunk = pickRawResponseItemText(params.item);
          const itemId = typeof params.item?.id === "string" ? params.item.id : "";
          if (textChunk && !(itemId && emittedItemIds.has(itemId)) && !emittedText) {
            emittedText += textChunk;
            if (itemId) emittedItemIds.add(itemId);
            yield { kind: "token", text: textChunk };
          }
          continue;
        }

        if (method === "error") {
          const message = pickAppServerErrorMessage(params) || "codex app-server reported an error";
          if (isUnsupportedCodexModelError(message)) {
            yield {
              kind: "error",
              code: "AGENT_UNSUPPORTED_MODEL",
              message: "Codex Desktop 续接该会话时仍返回模型不可用；HachimoDock 没有传模型参数，请确认 Desktop 当前会话本身能继续发送。",
            };
          } else {
            yield { kind: "error", code: pickAppServerErrorCode(params), message };
          }
          sawDone = true;
          break;
        }

        if (method === "turn/completed") {
          finalTurn = params.turn && typeof params.turn === "object" ? params.turn : null;
          const errorMessage = pickTurnErrorMessage(finalTurn);
          if (errorMessage) {
            if (isUnsupportedCodexModelError(errorMessage)) {
              yield {
                kind: "error",
                code: "AGENT_UNSUPPORTED_MODEL",
                message: "Codex Desktop 续接该会话时仍返回模型不可用；HachimoDock 没有传模型参数，请确认 Desktop 当前会话本身能继续发送。",
              };
            } else {
              yield { kind: "error", code: "AGENT_ERROR", message: errorMessage };
            }
            sawDone = true;
            break;
          }

          const finalText = pickTurnAgentText(finalTurn);
          if (finalText && !emittedText) {
            emittedText += finalText;
            yield { kind: "token", text: finalText };
          }
          sawDone = true;
          yield {
            kind: "done",
            sessionId: params.threadId || resumedThreadId,
            stopReason: "end_turn",
          };
          break;
        }
      }

      if (!sawDone) {
        const exit = await rpc.exit();
        if (exit.err) {
          yield { kind: "error", code: "SPAWN_FAILED", message: String(exit.err.message || exit.err) };
        } else if (exit.code !== 0 && exit.code !== null) {
          yield {
            kind: "error",
            code: "AGENT_EXIT_NONZERO",
            message: `codex app-server exited with code=${exit.code} sig=${exit.sig || ""}: ${rpc.stderr().trim().slice(0, 512)}`,
          };
        } else {
          const finalText = pickTurnAgentText(finalTurn);
          if (finalText && !emittedText) {
            yield { kind: "token", text: finalText };
          }
          yield { kind: "done", sessionId: sessionId, stopReason: "stream_ended" };
        }
      }
    } catch (error) {
      const message = error?.message || String(error);
      if (isUnsupportedCodexModelError(message)) {
        yield {
          kind: "error",
          code: "AGENT_UNSUPPORTED_MODEL",
          message: "Codex Desktop 续接该会话时仍返回模型不可用；HachimoDock 没有传模型参数，请确认 Desktop 当前会话本身能继续发送。",
        };
      } else {
        yield {
          kind: "error",
          code: error?.code || "AGENT_ERROR",
          message,
        };
      }
    } finally {
      if (aborter && signal) signal.removeEventListener("abort", aborter);
      await rpc.close();
    }
  }

  async *_spawnAndIterate({ bin, args, cwd, signal, fallbackSessionId }) {
    this.log("info", "spawning codex", { bin, args, cwd, hasSession: Boolean(fallbackSessionId) });
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

          const sid = pickSessionId(frame);
          if (sid && !resolvedSessionId) resolvedSessionId = sid;

          for (const evt of frameToEvents(frame)) {
            if (evt.kind === "done") {
              sawDone = true;
              yield { ...evt, sessionId: evt.sessionId || resolvedSessionId };
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
            message: `codex exited with code=${code} sig=${sig || ""}: ${stderrBuf.trim().slice(0, 512)}`,
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
    const sep = process.platform === "win32" ? ";" : ":";
    const extras = this._extraPathDirsOverride !== null
      ? this._extraPathDirsOverride
      : [
          `${home}/.npm-global/bin`,
          `${home}/.local/bin`,
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
  // Prefer Codex Desktop's unpacked runtime dirs first on Windows.
  // The Microsoft Store WindowsApps shim can exist but still fail to execute
  // with "Access is denied" in sidecar contexts.
  for (const dir of windowsCodexDesktopDirs(env)) {
    push(dir);
  }
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

function windowsCodexDesktopDirs(env) {
  const dirs = [];
  const push = (value) => {
    if (value && !dirs.includes(value)) dirs.push(value);
  };
  const local = env.LOCALAPPDATA;
  if (!local) return dirs;

  // Standalone desktop runtime.
  push(`${local}\\OpenAI\\Codex\\bin`);

  // Known default package family + dynamic package-family scan.
  const packagesRoot = `${local}\\Packages`;
  push(`${packagesRoot}\\OpenAI.Codex_2p2nqsd0c76g0\\LocalCache\\Local\\OpenAI\\Codex\\bin`);
  try {
    for (const entry of fs.readdirSync(packagesRoot, { withFileTypes: true })) {
      if (!entry || !entry.isDirectory() || !entry.name.startsWith("OpenAI.Codex_")) continue;
      push(`${packagesRoot}\\${entry.name}\\LocalCache\\Local\\OpenAI\\Codex\\bin`);
    }
  } catch {
    // Best-effort scan only.
  }

  // Some desktop builds keep codex.exe in hash-versioned child dirs.
  const snapshot = [...dirs];
  for (const base of snapshot) {
    try {
      for (const entry of fs.readdirSync(base, { withFileTypes: true })) {
        if (!entry || !entry.isDirectory()) continue;
        push(`${base}\\${entry.name}`);
      }
    } catch {
      // Best-effort scan only.
    }
  }
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
  // Codex 0.118+: { type: "thread.started", thread_id: "<uuid>" }
  if (typeof frame.thread_id === "string") return frame.thread_id;
  if (typeof frame.threadId === "string") return frame.threadId;
  return "";
}

function pickExtraSystemPrompt(metadata) {
  if (!metadata || typeof metadata !== "object") return "";
  const v = metadata.extraSystemPrompt;
  return typeof v === "string" && v.trim() ? v.trim() : "";
}

function isUnsupportedCodexModelError(message) {
  if (typeof message !== "string") return false;
  const normalized = message.toLowerCase();
  return normalized.includes("model is not supported")
    || normalized.includes("model") && normalized.includes("chatgpt account");
}

function createStdioJsonRpcClient({ child, log, stderrLimit = 8192 }) {
  let nextId = 1;
  let stdoutBuf = "";
  let stderrBuf = "";
  let closed = false;
  const pending = new Map();
  const notifications = [];
  let notifyWake = null;

  const exitPromise = new Promise((resolveExit) => {
    child.once("close", (code, sig) => {
      closed = true;
      resolveExit({ code, sig });
      wakeNotificationWaiter();
      for (const [, p] of pending) {
        p.reject(Object.assign(new Error(`codex app-server exited with code=${code} sig=${sig || ""}: ${stderrBuf.trim().slice(0, 512)}`), {
          code: "AGENT_EXIT_NONZERO",
        }));
      }
      pending.clear();
    });
    child.once("error", (err) => {
      closed = true;
      resolveExit({ code: -1, sig: null, err });
      wakeNotificationWaiter();
      for (const [, p] of pending) {
        p.reject(Object.assign(new Error(String(err?.message || err)), { code: "SPAWN_FAILED" }));
      }
      pending.clear();
    });
  });

  child.stdout.setEncoding("utf8");
  child.stdout.on("data", (chunk) => {
    stdoutBuf += chunk;
    let idx;
    while ((idx = stdoutBuf.indexOf("\n")) >= 0) {
      const line = stdoutBuf.slice(0, idx);
      stdoutBuf = stdoutBuf.slice(idx + 1);
      handleLine(line);
    }
  });
  child.stdout.on("end", () => {
    if (stdoutBuf.trim()) handleLine(stdoutBuf);
    stdoutBuf = "";
  });

  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk) => {
    stderrBuf += chunk;
    if (stderrBuf.length > stderrLimit) stderrBuf = stderrBuf.slice(-stderrLimit);
  });

  function handleLine(line) {
    const frame = parseLine(line);
    if (!frame || typeof frame !== "object") return;
    const hasId = Object.prototype.hasOwnProperty.call(frame, "id");
    const isRequest = hasId && typeof frame.method === "string";
    if (isRequest) {
      respondToServerRequest(frame);
      return;
    }
    if (hasId) {
      const p = pending.get(frame.id);
      if (!p) return;
      pending.delete(frame.id);
      if (frame.error) {
        p.reject(Object.assign(new Error(pickRpcErrorMessage(frame.error)), {
          code: pickRpcErrorCode(frame.error),
          details: frame.error,
        }));
      } else {
        p.resolve(frame.result);
      }
      return;
    }
    if (typeof frame.method === "string") {
      notifications.push(frame);
      wakeNotificationWaiter();
    }
  }

  function writeJson(obj) {
    if (closed || child.stdin.destroyed) {
      throw Object.assign(new Error("codex app-server stdin is closed"), { code: "SPAWN_CLOSED" });
    }
    child.stdin.write(`${JSON.stringify(obj)}\n`, "utf8");
  }

  function respondToServerRequest(frame) {
    const result = defaultServerRequestResult(frame.method);
    if (result !== undefined) {
      try {
        writeJson({ id: frame.id, result });
      } catch (error) {
        log?.("warn", "failed to respond to codex app-server request", {
          method: frame.method,
          error: String(error?.message || error),
        });
      }
      return;
    }
    try {
      writeJson({
        id: frame.id,
        error: {
          code: "UNSUPPORTED_CLIENT_REQUEST",
          message: `HachimoDock cannot handle codex app-server request ${frame.method}`,
        },
      });
    } catch {
      /* ignore */
    }
  }

  function request(method, params, { timeoutMs = 120000 } = {}) {
    const id = nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        pending.delete(id);
        reject(Object.assign(new Error(`codex app-server request timed out: ${method}`), { code: "REQUEST_TIMEOUT" }));
      }, timeoutMs);
      pending.set(id, {
        resolve: (value) => {
          clearTimeout(timer);
          resolve(value);
        },
        reject: (error) => {
          clearTimeout(timer);
          reject(error);
        },
      });
      try {
        writeJson({ id, method, params });
      } catch (error) {
        clearTimeout(timer);
        pending.delete(id);
        reject(error);
      }
    });
  }

  async function nextNotificationOrExit() {
    while (notifications.length === 0 && !closed) {
      await new Promise((resolve) => { notifyWake = resolve; });
    }
    if (notifications.length > 0) return notifications.shift();
    return null;
  }

  function wakeNotificationWaiter() {
    const wake = notifyWake;
    notifyWake = null;
    if (wake) wake();
  }

  async function close() {
    if (!closed) {
      try { child.stdin.end(); } catch { /* ignore */ }
      const killer = setTimeout(() => {
        if (!closed && !child.killed) {
          try { child.kill("SIGTERM"); } catch { /* ignore */ }
        }
      }, 50);
      try {
        await Promise.race([
          exitPromise,
          new Promise((resolve) => setTimeout(resolve, 1000)),
        ]);
      } finally {
        clearTimeout(killer);
      }
    }
    if (!closed && !child.killed) {
      try { child.kill("SIGTERM"); } catch { /* ignore */ }
      await Promise.race([
        exitPromise,
        new Promise((resolve) => setTimeout(resolve, 250)),
      ]);
    }
  }

  return {
    request,
    nextNotificationOrExit,
    exit: () => exitPromise,
    close,
    stderr: () => stderrBuf,
  };
}

function defaultServerRequestResult(method) {
  switch (method) {
    case "item/commandExecution/requestApproval":
    case "execCommandApproval":
      return { decision: "decline" };
    case "item/fileChange/requestApproval":
    case "applyPatchApproval":
      return { decision: "decline" };
    case "item/permissions/requestApproval":
      return { permissions: {}, scope: "turn" };
    case "item/tool/requestUserInput":
      return { answers: {} };
    case "mcpServer/elicitation/request":
      return { action: "decline", content: null };
    case "item/tool/call":
      return {
        success: false,
        contentItems: [{ type: "inputText", text: "HachimoDock cannot run this client-side tool." }],
      };
    default:
      return undefined;
  }
}

function pickRpcErrorMessage(error) {
  if (typeof error === "string") return error;
  if (typeof error?.message === "string") return error.message;
  return "codex app-server request failed";
}

function pickRpcErrorCode(error) {
  if (typeof error?.code === "string") return error.code;
  if (Number.isFinite(error?.code)) return String(error.code);
  return "AGENT_ERROR";
}

function pickThreadIdFromAppServerResult(result) {
  if (!result || typeof result !== "object") return "";
  if (typeof result.threadId === "string") return result.threadId;
  if (typeof result.thread?.id === "string") return result.thread.id;
  return "";
}

function pickThreadItemText(item) {
  if (!item || typeof item !== "object") return "";
  if (item.type === "agentMessage" && typeof item.text === "string") return item.text;
  return "";
}

function pickRawResponseItemText(item) {
  if (!item || typeof item !== "object") return "";
  if (item.type !== "message" || item.role !== "assistant" || !Array.isArray(item.content)) return "";
  return item.content
    .map((block) => block && block.type === "output_text" && typeof block.text === "string" ? block.text : "")
    .filter(Boolean)
    .join("");
}

function pickTurnAgentText(turn) {
  if (!turn || typeof turn !== "object" || !Array.isArray(turn.items)) return "";
  const messages = turn.items
    .map((item) => pickThreadItemText(item))
    .filter(Boolean);
  return messages.length ? messages[messages.length - 1] : "";
}

function pickTurnErrorMessage(turn) {
  if (!turn || typeof turn !== "object") return "";
  if (turn.status === "failed") {
    if (typeof turn.error?.message === "string") return turn.error.message;
    return "codex turn failed";
  }
  return "";
}

function appServerItemToToolEvents(item) {
  if (!item || typeof item !== "object") return [];
  const type = typeof item.type === "string" ? item.type : "";
  if (type === "commandExecution") {
    return [{
      kind: "tool",
      name: "shell",
      phase: item.status === "inProgress" ? "start" : "end",
      input: item.command,
      ok: item.status === "completed",
    }];
  }
  if (type === "mcpToolCall" || type === "dynamicToolCall" || type === "collabAgentToolCall") {
    return [{
      kind: "tool",
      name: typeof item.tool === "string" ? item.tool : "unknown",
      phase: item.status === "inProgress" ? "start" : "end",
      input: item.arguments,
      ok: item.status === "completed" || item.success === true,
    }];
  }
  return [];
}

function pickAppServerErrorMessage(params) {
  if (typeof params?.error?.message === "string") return params.error.message;
  if (typeof params?.message === "string") return params.message;
  return "";
}

function pickAppServerErrorCode(params) {
  const info = params?.error?.codexErrorInfo;
  if (typeof info === "string") return info;
  return "AGENT_ERROR";
}

module.exports = {
  CodexAdapter,
};
