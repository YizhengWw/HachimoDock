"use strict";

const { spawn } = require("child_process");

const { BaseAdapter } = require("./base");
const { findExecutable, readVersion } = require("../util/binary-resolver");

const LIST_TIMEOUT_MS = 12000;
const MAX_OUTPUT_BYTES = 2 * 1024 * 1024;

class MiMoCodeAdapter extends BaseAdapter {
  constructor({
    log,
    env,
    cwd,
    fallbackPaths,
    extraPathDirs,
  } = {}) {
    super({ agentId: "mimocode", log });
    this._env = env || process.env;
    this._cwd = cwd || process.cwd();
    this._fallbackPathsOverride = Array.isArray(fallbackPaths) ? fallbackPaths : null;
    this._extraPathDirsOverride = Array.isArray(extraPathDirs) ? extraPathDirs : null;
    this._cachedBin = null;
    this._cachedAvailability = null;
    this._cachedAt = 0;
  }

  _defaultFallbackPaths() {
    const home = this._env.HOME || this._env.USERPROFILE || "~";
    return [
      `${home}/.mimocode/bin/mimo`,
      `${home}/.mimocode/bin/mimo.exe`,
      `${home}/.local/bin/mimo`,
    ];
  }

  _defaultExtraPathDirs() {
    const home = this._env.HOME || this._env.USERPROFILE || "~";
    return [
      `${home}/.mimocode/bin`,
      `${home}/.local/bin`,
    ];
  }

  _resolveBinary() {
    if (this._cachedBin) return this._cachedBin;
    this._cachedBin = findExecutable({
      binName: "mimo",
      envVar: "MIMOCODE_CLI_PATH",
      env: this._env,
      fallbackPaths: this._fallbackPathsOverride !== null
        ? this._fallbackPathsOverride
        : this._defaultFallbackPaths(),
      extraPathDirs: this._extraPathDirsOverride !== null
        ? this._extraPathDirsOverride
        : this._defaultExtraPathDirs(),
    });
    return this._cachedBin;
  }

  async isAvailable() {
    const now = Date.now();
    if (this._cachedAvailability && now - this._cachedAt < 5000) {
      return this._cachedAvailability;
    }

    const bin = this._resolveBinary();
    let value;
    if (!bin) {
      value = {
        ready: false,
        reason: "MiMoCode CLI 未找到（请安装 MiMoCode 或设置 MIMOCODE_CLI_PATH）",
      };
    } else {
      const version = readVersion(bin, { env: this._env });
      value = version
        ? { ready: true, reason: null, version }
        : { ready: false, reason: `mimo --version 调用失败 (${bin})` };
    }
    this._cachedAvailability = value;
    this._cachedAt = now;
    return value;
  }

  async listSessions({ limit = 20 } = {}) {
    const probe = await this.isAvailable();
    if (!probe.ready) return [];
    const bin = this._resolveBinary();
    if (!bin) return [];

    const safeLimit = Math.max(1, Math.min(200, Number.parseInt(limit, 10) || 20));
    const output = await collectProcessOutput(bin, [
      "session",
      "list",
      "--format",
      "json",
      "--max-count",
      String(safeLimit),
    ], {
      cwd: this._cwd,
      env: this._env,
      timeoutMs: LIST_TIMEOUT_MS,
    });
    if (!output.stdout.trim()) return [];

    const rows = parseSessionList(output.stdout);
    return rows
      .map(normalizeSession)
      .filter(Boolean)
      .sort((a, b) => b.lastModified - a.lastModified)
      .slice(0, safeLimit);
  }

  async openNew({ cwd } = {}) {
    return {
      id: "",
      lastModified: Date.now(),
      cwd: cwd || this._cwd,
      summary: "new (mimocode)",
    };
  }

  async *inject() {
    yield {
      kind: "error",
      code: "AGENT_INPUT_UNSUPPORTED",
      message: "MiMoCode 第一阶段仅同步状态与会话，设备语音输入将在下一阶段接入。",
    };
  }
}

function normalizeSession(row) {
  if (!row || typeof row !== "object") return null;
  const id = typeof row.id === "string" ? row.id.trim() : "";
  if (!id) return null;
  const title = typeof row.title === "string" && row.title.trim()
    ? row.title.trim()
    : "MiMoCode 会话";
  const updated = Number(row.updated);
  const created = Number(row.created);
  return {
    id,
    name: title,
    summary: title,
    cwd: typeof row.directory === "string" ? row.directory : undefined,
    lastModified: Number.isFinite(updated)
      ? updated
      : Number.isFinite(created)
        ? created
        : 0,
  };
}

function parseSessionList(stdout) {
  const text = String(stdout || "").trim();
  try {
    const value = JSON.parse(text);
    return Array.isArray(value) ? value : [];
  } catch {
    const start = text.indexOf("[");
    const end = text.lastIndexOf("]");
    if (start < 0 || end <= start) {
      throw new Error("MiMoCode session list 未返回 JSON 数组");
    }
    const value = JSON.parse(text.slice(start, end + 1));
    return Array.isArray(value) ? value : [];
  }
}

function collectProcessOutput(bin, args, options = {}) {
  return new Promise((resolve, reject) => {
    const useShell = process.platform === "win32" && /\.(cmd|bat)$/i.test(bin);
    const child = spawn(bin, args, {
      cwd: options.cwd,
      env: options.env,
      windowsHide: true,
      shell: useShell,
      stdio: ["ignore", "pipe", "pipe"],
    });
    let stdout = "";
    let stderr = "";
    let outputBytes = 0;
    let settled = false;
    let timer = null;

    const finish = (error, value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      if (error) reject(error);
      else resolve(value);
    };
    const append = (target, chunk) => {
      outputBytes += chunk.length;
      if (outputBytes > MAX_OUTPUT_BYTES) {
        child.kill();
        finish(new Error("MiMoCode session list 输出过大"));
        return target;
      }
      return target + chunk.toString("utf8");
    };
    child.stdout.on("data", (chunk) => { stdout = append(stdout, chunk); });
    child.stderr.on("data", (chunk) => { stderr = append(stderr, chunk); });
    child.once("error", (error) => finish(error));
    child.once("close", (code, signal) => {
      if (code === 0) {
        finish(null, { stdout, stderr });
        return;
      }
      finish(new Error(
        `mimo session list exited code=${code} signal=${signal || ""}: ${stderr.trim().slice(0, 500)}`,
      ));
    });

    timer = setTimeout(() => {
      child.kill();
      finish(new Error("mimo session list timed out"));
    }, options.timeoutMs || LIST_TIMEOUT_MS);
    timer.unref?.();
  });
}

module.exports = {
  MiMoCodeAdapter,
  _internal: {
    normalizeSession,
    parseSessionList,
  },
};
