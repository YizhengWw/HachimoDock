"use strict";

/*
 * [Input] Codex session directory trees, metadata-bearing JSONL rows, and legacy flat session files.
 * [Output] Active-session candidates that skip metadata-less rollout files before resume selection.
 * [Pos] Codex session discovery helper for agent-session-bus.
 * [Sync] If Codex session filtering changes, update `ref/.folder.md`.
 */

const fs = require("fs");
const os = require("os");
const path = require("path");

/**
 * Where Codex CLI stores per-session JSONL files.
 *
 * Layout (Codex CLI 0.118+, observed in 0.125):
 *   ~/.codex/sessions/<YYYY>/<MM>/<DD>/rollout-<iso>-<uuid>.jsonl
 *
 * Older / Codex Desktop builds occasionally flatten this to one or two
 * levels. We walk recursively up to a small max depth, which covers every
 * shape we've seen in the wild without scanning unrelated trees.
 *
 * @param {object} [opts]
 * @param {NodeJS.ProcessEnv} [opts.env]
 * @returns {string}
 */
function codexSessionsRoot({ env = process.env } = {}) {
  const home = env.CODEX_HOME || env.HOME || env.USERPROFILE || os.homedir();
  return path.join(home, ".codex", "sessions");
}

function codexSessionIndexPath({ env = process.env } = {}) {
  const explicit = env.CLAWD_CODEX_SESSION_INDEX_PATH || env.CODEX_SESSION_INDEX_PATH;
  if (explicit && String(explicit).trim()) return String(explicit).trim();
  return path.join(path.dirname(codexSessionsRoot({ env })), "session_index.jsonl");
}

function codexModelsCachePath({ env = process.env } = {}) {
  const explicit = env.CLAWD_CODEX_MODELS_CACHE_PATH || env.CODEX_MODELS_CACHE_PATH;
  if (explicit && String(explicit).trim()) return String(explicit).trim();
  return path.join(path.dirname(codexSessionsRoot({ env })), "models_cache.json");
}

/**
 * List Codex sessions sorted for voice `auto`: named client threads first by
 * rollout recency, then exec-only sessions by start time. Never throws.
 *
 * Each result carries:
 * - `id`     – the Codex thread/session UUID. Existing Desktop threads are
 *              continued through `codex app-server` with this id. We pull it
 *              from the jsonl `session_meta.payload.id` field; it is NOT
 *              necessarily the file basename, since the basename is
 *              `rollout-<iso>-<uuid>` with extra prefix bits.
 *              `rollout-*.jsonl` files without `session_meta` are skipped
 *              because Codex app-server refuses to resume them.
 * - `cwd`    – the workspace the session ran in (from
 *              `session_meta.payload.cwd`). Optional; older builds may
 *              omit it.
 * - `summary`– short preview of the first user turn, for UI listing.
 * - `name`   – Codex client's thread_name from session_index.jsonl.
 * - `createdAt` – best-effort session creation time, parsed from the
 *              rollout filename before falling back to file birth/mtime.
 * - `clientUpdatedAt` – Codex client's updated_at time when present; used
 *              only as a fallback because CLI resume does not reliably bump it.
 * - `model`  – most recent Codex turn_context model recorded in the session.
 * - `modelSupport` – whether that model is present in Codex's account-local
 *              models cache: "supported", "unsupported", or "unknown".
 *
 * @param {object} [opts]
 * @param {string} [opts.cwd]   if provided, restrict to sessions whose
 *                              recorded cwd matches (path-resolved equality)
 * @param {number} [opts.limit] cap the result; default 100
 * @param {NodeJS.ProcessEnv} [opts.env]
 * @returns {Array<{id: string, lastModified: number, createdAt: number, clientUpdatedAt?: number, cwd?: string, name?: string, summary?: string, model?: string, modelSupport?: "supported"|"unsupported"|"unknown", modelSupported?: boolean}>}
 */
function listCodexSessions({ cwd, limit = 100, env = process.env } = {}) {
  const root = codexSessionsRoot({ env });
  const index = readSessionIndex({ env });
  const availableModels = readAvailableCodexModels({ env });
  let files;
  try {
    files = collectJsonlFiles(root, /* maxDepth */ 4);
  } catch (error) {
    if (error && error.code === "ENOENT") return [];
    throw error;
  }

  const results = [];
  for (const fullPath of files) {
    let stat;
    try { stat = fs.statSync(fullPath); } catch { continue; }
    const basename = path.basename(fullPath);
    const meta = readSessionMeta(fullPath);
    // Codex Desktop's app-server rejects rollout files that do not begin with
    // session_meta. Do not treat those as resumable just because the basename
    // ends in a UUID; they are internal/non-session rollouts.
    if (isRolloutBasename(basename) && !(meta && meta.id)) continue;
    // Older flat layouts may be named directly after the session id. Keep the
    // basename fallback for those non-rollout files.
    const fallbackId = extractUuidFromBasename(basename);
    const id = (meta && meta.id) || fallbackId;
    if (!id) continue;
    const sessionCwd = meta && meta.cwd ? meta.cwd : "";
    if (cwd && sessionCwd && !sameCwd(cwd, sessionCwd)) continue;
    const createdAt = sessionCreatedAt(fullPath, stat);
    const indexed = index.get(id) || null;
    const model = readSessionRecentModel(fullPath, stat) || (meta && meta.model) || "";
    const modelSupport = classifyModelSupport(model, availableModels);
    results.push({
      id,
      lastModified: stat.mtimeMs,
      createdAt,
      ...(indexed && indexed.name ? { name: indexed.name } : {}),
      ...(indexed && indexed.updatedAt ? { clientUpdatedAt: indexed.updatedAt } : {}),
      ...(sessionCwd ? { cwd: sessionCwd } : {}),
      ...(meta && meta.summary ? { summary: meta.summary } : {}),
      ...(model ? { model } : {}),
      modelSupport,
      ...(modelSupport !== "unknown" ? { modelSupported: modelSupport === "supported" } : {}),
    });
  }

  results.sort((a, b) => {
    const namedDelta = Number(Boolean(b.name)) - Number(Boolean(a.name));
    if (namedDelta !== 0) return namedDelta;
    if (a.name && b.name) {
      const modifiedDelta = (b.lastModified || 0) - (a.lastModified || 0);
      if (modifiedDelta !== 0) return modifiedDelta;
      const clientDelta = (b.clientUpdatedAt || 0) - (a.clientUpdatedAt || 0);
      if (clientDelta !== 0) return clientDelta;
    }
    const createdDelta = (b.createdAt || 0) - (a.createdAt || 0);
    if (createdDelta !== 0) return createdDelta;
    return b.lastModified - a.lastModified;
  });
  return results.slice(0, limit);
}

function readAvailableCodexModels({ env = process.env } = {}) {
  let raw = "";
  try {
    raw = fs.readFileSync(codexModelsCachePath({ env }), "utf8");
  } catch {
    return new Set();
  }

  let obj;
  try {
    obj = JSON.parse(raw);
  } catch {
    return new Set();
  }

  const out = new Set();
  const models = Array.isArray(obj && obj.models) ? obj.models : [];
  for (const row of models) {
    const slug = typeof row?.slug === "string" ? row.slug.trim() : "";
    if (slug) out.add(slug);
  }
  return out;
}

function readSessionIndex({ env = process.env } = {}) {
  const out = new Map();
  let raw = "";
  try {
    raw = fs.readFileSync(codexSessionIndexPath({ env }), "utf8");
  } catch {
    return out;
  }
  for (const line of raw.split(/\r?\n/)) {
    if (!line.trim()) continue;
    let obj;
    try { obj = JSON.parse(line); } catch { continue; }
    const id = typeof obj.id === "string" ? obj.id.trim() : "";
    if (!id) continue;
    const name = typeof obj.thread_name === "string" ? obj.thread_name.trim() : "";
    const updatedAtText = typeof obj.updated_at === "string" ? obj.updated_at.trim() : "";
    const updatedAt = updatedAtText ? Date.parse(updatedAtText) : 0;
    out.set(id, {
      name,
      updatedAt: Number.isFinite(updatedAt) && updatedAt > 0 ? updatedAt : 0,
    });
  }
  return out;
}

/**
 * Walk `dir` recursively (bounded by `maxDepth`) and return absolute paths
 * to every `*.jsonl` file. Tolerates ENOENT at the root (returns []) but
 * propagates other I/O errors.
 *
 * @param {string} dir
 * @param {number} maxDepth
 * @returns {string[]}
 */
function collectJsonlFiles(dir, maxDepth) {
  const out = [];
  /** @type {Array<{ p: string; depth: number }>} */
  const stack = [{ p: dir, depth: 0 }];
  while (stack.length) {
    const { p, depth } = stack.pop();
    let entries;
    try {
      entries = fs.readdirSync(p, { withFileTypes: true });
    } catch (error) {
      // Bubble ENOENT for the root only — for nested missing dirs, just skip.
      if (depth === 0) throw error;
      continue;
    }
    for (const entry of entries) {
      const full = path.join(p, entry.name);
      if (entry.isFile()) {
        if (entry.name.endsWith(".jsonl")) out.push(full);
      } else if (entry.isDirectory() && depth < maxDepth) {
        stack.push({ p: full, depth: depth + 1 });
      }
    }
  }
  return out;
}

/**
 * Pull `id`, `cwd`, `model`, and a short user-facing `summary` out of a Codex
 * session jsonl. Codex's first line is a `session_meta` event whose
 * `payload` carries both the canonical session UUID and the cwd:
 *
 *   {"type":"session_meta","payload":{"id":"<uuid>","cwd":"/...","..."},...}
 *
 * `summary` is best-effort: the first user message after the meta line
 * (if we encounter one within the read budget). Returns null on any
 * I/O or parse failure — caller should fall back to filename-derived
 * defaults.
 *
 * Read budget is 256 KiB which is enough to clear the giant base
 * instructions blob Codex Desktop dumps into the meta line on launch.
 *
 * @param {string} filePath
 * @returns {{ id?: string, cwd?: string, model?: string, summary?: string } | null}
 */
function readSessionMeta(filePath) {
  let fd;
  try {
    fd = fs.openSync(filePath, "r");
    const buf = Buffer.alloc(256 * 1024);
    const bytes = fs.readSync(fd, buf, 0, buf.length, 0);
    const text = buf.subarray(0, bytes).toString("utf8");
    let id = "";
    let cwd = "";
    let model = "";
    let summary = "";
    let offset = 0;
    while (offset < text.length) {
      const nl = text.indexOf("\n", offset);
      const line = nl === -1 ? text.slice(offset) : text.slice(offset, nl);
      offset = nl === -1 ? text.length : nl + 1;
      if (!line.trim()) {
        if (nl === -1) break;
        continue;
      }
      let obj;
      try { obj = JSON.parse(line); } catch { if (nl === -1) break; else continue; }
      if (obj && obj.type === "session_meta" && obj.payload) {
        if (typeof obj.payload.id === "string") id = obj.payload.id;
        if (typeof obj.payload.cwd === "string") cwd = obj.payload.cwd;
      }
      const turnModel = pickTurnContextModel(obj);
      if (!model && turnModel) model = turnModel;
      if (!summary && obj && obj.type === "response_item" && obj.payload) {
        const text = pickFirstUserText(obj.payload);
        if (text) summary = text.slice(0, 80);
      }
      if (id && cwd && model && summary) break;
      if (nl === -1) break;
    }
    if (!id && !cwd) return null;
    return { id, cwd, model, summary };
  } catch {
    return null;
  } finally {
    if (fd !== undefined) {
      try { fs.closeSync(fd); } catch { /* ignore */ }
    }
  }
}

function readSessionRecentModel(filePath, stat) {
  let fd;
  try {
    const size = Math.max(0, Number(stat && stat.size) || fs.statSync(filePath).size || 0);
    const budget = Math.min(size, 1024 * 1024);
    const start = Math.max(0, size - budget);
    fd = fs.openSync(filePath, "r");
    const buf = Buffer.alloc(budget);
    const bytes = fs.readSync(fd, buf, 0, budget, start);
    let text = buf.subarray(0, bytes).toString("utf8");
    if (start > 0) {
      const nl = text.indexOf("\n");
      text = nl === -1 ? "" : text.slice(nl + 1);
    }

    let model = "";
    for (const line of text.split(/\r?\n/)) {
      if (!line.trim()) continue;
      let obj;
      try { obj = JSON.parse(line); } catch { continue; }
      const turnModel = pickTurnContextModel(obj);
      if (turnModel) model = turnModel;
    }
    return model;
  } catch {
    return "";
  } finally {
    if (fd !== undefined) {
      try { fs.closeSync(fd); } catch { /* ignore */ }
    }
  }
}

function pickTurnContextModel(obj) {
  if (!obj || obj.type !== "turn_context" || !obj.payload) return "";
  const model = typeof obj.payload.model === "string" ? obj.payload.model.trim() : "";
  return model;
}

function classifyModelSupport(model, availableModels) {
  if (!model || !(availableModels instanceof Set) || availableModels.size === 0) return "unknown";
  return availableModels.has(model) ? "supported" : "unsupported";
}

/**
 * Heuristic: pull a UUID out of `rollout-2026-04-03T18-38-50-019d52ec-97bd-78e2-a696-5893573eb93b.jsonl`.
 * Falls back to the bare basename for older flat layouts.
 *
 * @param {string} basename
 * @returns {string}
 */
function extractUuidFromBasename(basename) {
  const noExt = basename.replace(/\.jsonl$/, "");
  // Match the trailing canonical UUID. Codex 0.40 used `<uuid>.jsonl`;
  // Codex 0.118+ uses `rollout-<iso>-<uuid>.jsonl`. The trailing-uuid match
  // covers both.
  const match = noExt.match(/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i);
  if (match) return match[0];
  return noExt;
}

function isRolloutBasename(basename) {
  return /^rollout-/i.test(String(basename || ""));
}

function sessionCreatedAt(filePath, stat) {
  const parsed = parseRolloutTimestamp(path.basename(filePath));
  if (parsed > 0) return parsed;
  const mtime = Number(stat && stat.mtimeMs);
  if (Number.isFinite(mtime) && mtime > 0) return mtime;
  const birthtime = Number(stat && stat.birthtimeMs);
  return Number.isFinite(birthtime) && birthtime > 0 ? birthtime : 0;
}

function parseRolloutTimestamp(basename) {
  const match = String(basename || "").match(
    /^rollout-(\d{4})-(\d{2})-(\d{2})T(\d{2})-(\d{2})-(\d{2})-/,
  );
  if (!match) return 0;
  const [, year, month, day, hour, minute, second] = match;
  const ms = Date.UTC(
    Number(year),
    Number(month) - 1,
    Number(day),
    Number(hour),
    Number(minute),
    Number(second),
  );
  return Number.isFinite(ms) ? ms : 0;
}

/**
 * Codex stores `response_item` payloads with a polymorphic shape; only
 * scrape user-role text. Best-effort, returns "" if the payload doesn't
 * look like a user turn.
 *
 * @param {any} payload
 * @returns {string}
 */
function pickFirstUserText(payload) {
  if (!payload || typeof payload !== "object") return "";
  if (payload.type === "message" && payload.role === "user" && Array.isArray(payload.content)) {
    for (const c of payload.content) {
      if (!c || typeof c.text !== "string") continue;
      const text = summarizeUserText(c.text);
      if (text) return text;
    }
  }
  return "";
}

function summarizeUserText(value) {
  const text = typeof value === "string" ? value.trim() : "";
  if (!text) return "";
  if (text.startsWith("# AGENTS.md instructions")) return "";
  if (text.startsWith("<environment_context>")) return "";
  if (text.startsWith("<INSTRUCTIONS>")) return "";
  return text;
}

function sameCwd(a, b) {
  try {
    return path.resolve(a) === path.resolve(b);
  } catch {
    return a === b;
  }
}

module.exports = {
  codexModelsCachePath,
  codexSessionIndexPath,
  codexSessionsRoot,
  readAvailableCodexModels,
  listCodexSessions,
};
