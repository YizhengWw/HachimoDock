"use strict";

const fs = require("fs");
const os = require("os");
const path = require("path");
const { execSync } = require("child_process");

/**
 * Locate OpenClaw's installed `openclaw` npm package and its
 * `dist/plugin-sdk/agent-runtime.js` module.
 *
 * Mirrors the strategy used by `pet-claw-path-resolver.js` (which has been
 * battle-tested across npm/pnpm/yarn/nvm/homebrew installs) but is
 * dependency-free and read-only.
 *
 * Resolution order:
 *   1. Env var `OPENCLAW_RUNTIME_MODULE` pointing directly at the file.
 *   2. Env var `OPENCLAW_HOME` pointing at the package root.
 *   3. `npm root -g` + `pnpm root -g` + `yarn global dir` results.
 *   4. A fixed list of known global installation paths.
 *
 * Returns the absolute path to `agent-runtime.js`, or `null` if openclaw
 * is not installed on this machine.
 *
 * @param {object} [opts]
 * @param {NodeJS.ProcessEnv} [opts.env]
 * @returns {{ packageRoot: string, runtimeModule: string, packageVersion: string | null } | null}
 */
function locateOpenClaw({ env = process.env } = {}) {
  const home = env.OPENCLAW_HOME && env.OPENCLAW_HOME.trim()
    ? expandHome(env.OPENCLAW_HOME, env)
    : (env.HOME || env.USERPROFILE || os.homedir());

  const explicitModule = (env.OPENCLAW_RUNTIME_MODULE || "").trim();
  if (explicitModule && fs.existsSync(explicitModule)) {
    const packageRoot = path.resolve(path.dirname(explicitModule), "..", "..");
    const pkg = readPkg(packageRoot);
    return {
      packageRoot,
      runtimeModule: path.normalize(explicitModule),
      packageVersion: pkg?.version || null,
    };
  }

  const explicitHome = (env.OPENCLAW_HOME || "").trim();
  if (explicitHome) {
    const got = tryPackageRoot(expandHome(explicitHome, env));
    if (got) return got;
  }

  for (const cmd of ["npm root -g", "pnpm root -g"]) {
    const root = runCmd(cmd, env);
    if (!root) continue;
    const got = tryPackageRoot(path.join(root, "openclaw"));
    if (got) return got;
  }

  const yarnDir = runCmd("yarn global dir", env);
  if (yarnDir) {
    const got = tryPackageRoot(path.join(yarnDir, "node_modules", "openclaw"));
    if (got) return got;
  }

  const knownPaths = [
    path.join(home, ".npm-global", "lib", "node_modules", "openclaw"),
    path.join(home, ".npm-global", "node_modules", "openclaw"),
    path.join(home, "Library", "pnpm", "global", "5", "node_modules", "openclaw"),
    path.join(home, ".local", "share", "pnpm", "global", "5", "node_modules", "openclaw"),
    "/opt/homebrew/lib/node_modules/openclaw",
    "/usr/local/lib/node_modules/openclaw",
    "/usr/lib/node_modules/openclaw",
    path.join(home, ".nvm", "versions", "node", process.version, "lib", "node_modules", "openclaw"),
    ...(env.APPDATA ? [path.join(env.APPDATA, "npm", "node_modules", "openclaw")] : []),
    ...(env.LOCALAPPDATA ? [
      path.join(env.LOCALAPPDATA, "pnpm", "global", "5", "node_modules", "openclaw"),
    ] : []),
    ...(env.USERPROFILE ? [path.join(env.USERPROFILE, "AppData", "Roaming", "npm", "node_modules", "openclaw")] : []),
  ];
  for (const root of knownPaths) {
    const got = tryPackageRoot(root);
    if (got) return got;
  }

  return null;
}

function tryPackageRoot(packageRoot) {
  if (!packageRoot) return null;
  const pkg = readPkg(packageRoot);
  if (!pkg || pkg.name !== "openclaw") return null;
  const runtimeModule = path.join(packageRoot, "dist", "plugin-sdk", "agent-runtime.js");
  if (!fs.existsSync(runtimeModule)) return null;
  return {
    packageRoot: path.normalize(packageRoot),
    runtimeModule: path.normalize(runtimeModule),
    packageVersion: pkg.version || null,
  };
}

function readPkg(packageRoot) {
  try {
    const raw = fs.readFileSync(path.join(packageRoot, "package.json"), "utf8");
    return JSON.parse(raw);
  } catch {
    return null;
  }
}

function runCmd(command, env) {
  try {
    const out = execSync(command, {
      encoding: "utf8",
      windowsHide: true,
      timeout: 4000,
      stdio: ["ignore", "pipe", "ignore"],
      env: env || process.env,
    });
    return String(out).trim();
  } catch {
    return "";
  }
}

function expandHome(p, env) {
  if (!p) return p;
  const home = env.HOME || env.USERPROFILE || os.homedir();
  if (p === "~") return home;
  if (p.startsWith("~/") || p.startsWith("~\\")) return path.join(home, p.slice(2));
  return path.normalize(p);
}

/**
 * Where OpenClaw stores per-agent session state.
 * Layout: ~/.openclaw/agents/<agentId>/sessions/sessions.json
 *                                       /sessions/<rolloutId>.jsonl
 *
 * We read sessions.json for the list of session ids ordered by recency.
 *
 * @param {object} [opts]
 * @param {string} [opts.agentId]   defaults to "main"
 * @param {NodeJS.ProcessEnv} [opts.env]
 * @returns {Array<{id:string, lastModified:number}>}
 */
function listOpenClawSessions({ agentId = "main", env = process.env } = {}) {
  const home = env.OPENCLAW_STATE_DIR && env.OPENCLAW_STATE_DIR.trim()
    ? expandHome(env.OPENCLAW_STATE_DIR, env)
    : path.join(env.HOME || env.USERPROFILE || os.homedir(), ".openclaw");
  const dir = path.join(home, "agents", agentId, "sessions");
  let stat;
  try { stat = fs.statSync(dir); } catch { return []; }
  if (!stat.isDirectory()) return [];

  // Prefer the sessions.json index when present (it carries the canonical
  // last-touched-at). Fall back to mtime-sort the .jsonl files.
  const indexPath = path.join(dir, "sessions.json");
  if (fs.existsSync(indexPath)) {
    try {
      const raw = fs.readFileSync(indexPath, "utf8");
      const data = JSON.parse(raw);
      if (Array.isArray(data?.sessions)) {
        return data.sessions
          .map((s) => ({
            id: String(s.id || s.sessionId || ""),
            lastModified: Number(s.updatedAt || s.lastTouchedAt || s.createdAt || 0),
          }))
          .filter((s) => s.id)
          .sort((a, b) => b.lastModified - a.lastModified);
      }
    } catch {
      // fall through to mtime walk
    }
  }

  let entries;
  try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return []; }
  const out = [];
  for (const entry of entries) {
    if (!entry.isFile()) continue;
    if (!entry.name.endsWith(".jsonl")) continue;
    const full = path.join(dir, entry.name);
    let st;
    try { st = fs.statSync(full); } catch { continue; }
    out.push({ id: entry.name.replace(/\.jsonl$/, ""), lastModified: st.mtimeMs });
  }
  return out.sort((a, b) => b.lastModified - a.lastModified);
}

module.exports = {
  locateOpenClaw,
  listOpenClawSessions,
  // exported for tests
  _internal: { tryPackageRoot, expandHome },
};
