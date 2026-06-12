"use strict";

const fs = require("fs");
const os = require("os");
const path = require("path");
const { spawnSync } = require("child_process");

/**
 * Locate an executable across the wide variety of places different package
 * managers and ad-hoc installers leave it. The order of precedence is:
 *
 *   1. An explicit env var override (highest priority — power users always win).
 *   2. The current PATH (`which`/`where`), but ONLY after we augment it with
 *      well-known fallback dirs because Tauri-launched processes inherit a
 *      stripped-down PATH on macOS that often misses npm-global/homebrew.
 *   3. Each fallback path checked individually, expanding ~ in the process.
 *
 * If nothing matches we return null. Callers should treat that as
 * "agent not installed", not as an error.
 *
 * @typedef {object} ResolveOpts
 * @property {string} binName             e.g. "claude"
 * @property {string} [envVar]            e.g. "CLAUDE_CLI_PATH"
 * @property {string[]} [fallbackPaths]   ordered list, may use ~
 * @property {string[]} [extraPathDirs]   dirs to merge into PATH for `which`
 * @property {NodeJS.ProcessEnv} [env]    inject for tests; defaults to process.env
 * @property {NodeJS.Platform} [platform] inject for tests; defaults to process.platform
 *
 * @param {ResolveOpts} opts
 * @returns {string | null}
 */
function findExecutable(opts) {
  const env = opts.env || process.env;
  const platform = opts.platform || process.platform;
  const home = env.HOME || env.USERPROFILE || os.homedir();

  if (opts.envVar) {
    const override = (env[opts.envVar] || "").trim();
    if (override) {
      const expanded = expandHome(override, home);
      if (looksExecutable(expanded, platform)) return expanded;
    }
  }

  const augmentedPath = augmentPath(env.PATH || "", opts.extraPathDirs || [], home, platform);
  const viaPath = whichOnAugmentedPath(opts.binName, augmentedPath, platform, env);
  if (viaPath) return viaPath;

  for (const candidate of opts.fallbackPaths || []) {
    const expanded = expandHome(candidate, home);
    if (looksExecutable(expanded, platform)) return expanded;
  }

  return null;
}

/**
 * Resolve the version string of a CLI by spawning it with `--version`.
 * Returns null on any failure (binary not found, exit code != 0, no
 * recognizable version in stdout, etc). Never throws.
 *
 * @param {string} binPath
 * @param {object} [opts]
 * @param {number} [opts.timeoutMs=4000]
 * @param {string[]} [opts.args]   defaults to ["--version"]
 * @param {NodeJS.ProcessEnv} [opts.env]   env to pass to the spawned process
 * @returns {string | null}
 */
function readVersion(binPath, opts = {}) {
  if (!binPath || !looksExecutable(binPath)) return null;
  try {
    const useWindowsCmdShell = process.platform === "win32" && /\.(cmd|bat)$/i.test(binPath);
    const result = spawnSync(binPath, opts.args || ["--version"], {
      encoding: "utf8",
      timeout: opts.timeoutMs || 4000,
      windowsHide: true,
      stdio: ["ignore", "pipe", "pipe"],
      env: opts.env || process.env,
      shell: useWindowsCmdShell,
    });
    if (result.error || result.status !== 0) return null;
    const match = `${result.stdout || ""}\n${result.stderr || ""}`.match(/(\d+\.\d+(?:\.\d+)?)/);
    return match ? match[1] : null;
  } catch {
    return null;
  }
}

/**
 * Compare semver-ish "X.Y.Z" strings. Returns -1 / 0 / +1 like a comparator.
 * Missing components (e.g. "1.2") are treated as zero.
 */
function compareVersions(a, b) {
  const pa = String(a).split(".").map((p) => Number.parseInt(p, 10) || 0);
  const pb = String(b).split(".").map((p) => Number.parseInt(p, 10) || 0);
  const len = Math.max(pa.length, pb.length);
  for (let i = 0; i < len; i++) {
    const x = pa[i] || 0;
    const y = pb[i] || 0;
    if (x < y) return -1;
    if (x > y) return 1;
  }
  return 0;
}

function expandHome(p, home) {
  if (!p) return p;
  if (p === "~") return home;
  if (p.startsWith("~/")) return path.join(home, p.slice(2));
  if (p.startsWith("~\\")) return path.join(home, p.slice(2));
  return path.normalize(p);
}

function looksExecutable(p, platform = process.platform) {
  if (!p) return false;
  try {
    const stat = fs.statSync(p);
    if (!stat.isFile()) return false;
    if (platform === "win32") return true;
    fs.accessSync(p, fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

function augmentPath(currentPath, extra, home, platform) {
  const sep = platform === "win32" ? ";" : ":";
  const current = currentPath.split(sep).filter(Boolean);
  const expanded = (extra || []).map((p) => expandHome(p, home));
  // dedupe while preserving order: extra first (so they take precedence in
  // resolution order — power users symlinking into ~/.local/bin shouldn't be
  // shadowed by stale homebrew installs).
  const seen = new Set();
  const out = [];
  for (const dir of [...expanded, ...current]) {
    if (!dir) continue;
    if (seen.has(dir)) continue;
    seen.add(dir);
    out.push(dir);
  }
  return out.join(sep);
}

function whichOnAugmentedPath(binName, augmentedPath, platform, env = process.env) {
  const sep = platform === "win32" ? ";" : ":";
  const hasKnownExtension = /\.[a-z0-9]+$/i.test(binName);
  const exts = platform === "win32"
    ? (hasKnownExtension ? [""] : (env.PATHEXT || ".COM;.EXE;.BAT;.CMD").split(";").filter(Boolean))
    : [""];
  for (const dir of augmentedPath.split(sep)) {
    if (!dir) continue;
    for (const ext of exts) {
      const candidate = path.join(dir, binName + ext);
      if (looksExecutable(candidate, platform)) return candidate;
    }
  }
  return null;
}

module.exports = {
  findExecutable,
  readVersion,
  compareVersions,
  // exported for tests
  _internal: { expandHome, looksExecutable, augmentPath, whichOnAugmentedPath },
};
