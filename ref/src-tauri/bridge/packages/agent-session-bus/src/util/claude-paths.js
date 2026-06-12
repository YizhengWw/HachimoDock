"use strict";

const fs = require("fs");
const os = require("os");
const path = require("path");

/**
 * Where Claude Code stores per-cwd session JSONL files.
 * `~/.claude/projects/<encoded-cwd>/<sid>.jsonl`
 *
 * The encoded-cwd is the URL-safe form Claude Code itself uses internally;
 * we don't try to compute it ourselves (Anthropic's encoding has changed
 * subtly across versions). Instead we walk the projects dir and let the
 * sub-folder names speak for themselves.
 *
 * @param {object} [opts]
 * @param {NodeJS.ProcessEnv} [opts.env]
 * @returns {string}
 */
function claudeProjectsRoot({ env = process.env } = {}) {
  const home = env.CLAUDE_HOME || env.HOME || env.USERPROFILE || os.homedir();
  return path.join(home, ".claude", "projects");
}

/**
 * List all session JSONL refs across all projects, sorted newest-first.
 * Never throws — returns [] if the projects root doesn't exist or can't
 * be read.
 *
 * @param {object} [opts]
 * @param {string} [opts.cwd]   if provided, restrict to the project dir
 *                              that mirrors this cwd; if not, walk all
 * @param {number} [opts.limit] cap the result; default 100
 * @param {NodeJS.ProcessEnv} [opts.env]
 * @returns {Array<{id: string, lastModified: number, cwd?: string, name?: string, summary?: string}>}
 */
function listClaudeSessions({ cwd, limit = 100, env = process.env } = {}) {
  const root = claudeProjectsRoot({ env });
  let projects;
  try {
    projects = fs.readdirSync(root, { withFileTypes: true });
  } catch (error) {
    if (error && error.code === "ENOENT") return [];
    throw error;
  }

  const results = [];
  for (const entry of projects) {
    if (!entry.isDirectory()) continue;
    const projectDir = path.join(root, entry.name);
    const decodedCwd = decodeProjectDirName(entry.name);

    if (cwd && decodedCwd && !sameCwd(cwd, decodedCwd)) continue;

    let files;
    try {
      files = fs.readdirSync(projectDir, { withFileTypes: true });
    } catch {
      continue;
    }

    for (const file of files) {
      if (!file.isFile()) continue;
      if (!file.name.endsWith(".jsonl")) continue;
      const full = path.join(projectDir, file.name);
      let stat;
      try { stat = fs.statSync(full); } catch { continue; }
      // Reading cwd from the directory name is unreliable because Claude's
      // encoding replaces `/` with `-` *and* leaves any literal `-` in the
      // original path as `-` too — there's no way to round-trip
      // `-Users-petagent-work-HachimoDock-ref` back to
      // `/Users/petagent/work/HachimoDock/ref` from the folder name alone.
      // The session jsonl, however, embeds the real cwd in a top-level
      // `cwd` field on most lines. Read the first line and trust that.
      const meta = readSessionMetaFromJsonl(full);
      results.push({
        id: file.name.replace(/\.jsonl$/, ""),
        lastModified: stat.mtimeMs,
        cwd: meta.cwd || decodedCwd || undefined,
        ...(meta.name ? { name: meta.name } : {}),
        ...(meta.summary ? { summary: meta.summary } : {}),
      });
    }
  }

  results.sort((a, b) => b.lastModified - a.lastModified);
  return results.slice(0, limit);
}

/**
 * Pull user-facing metadata out of a Claude session jsonl. Claude prefixes
 * most lines with `{"cwd":"...","type":"...",...}` and newer builds may
 * include a client-generated title/name field. We prefer that title for the
 * UI and keep the first real user turn as a summary fallback.
 *
 * Read just the first 64 KiB; that's enough to capture the system init
 * line on every Claude version we've seen, and it bounds the cost when
 * we list lots of sessions.
 *
 * @param {string} filePath
 * @returns {{ cwd: string, name: string, summary: string }}
 */
function readSessionMetaFromJsonl(filePath) {
  let fd;
  try {
    fd = fs.openSync(filePath, "r");
    const buf = Buffer.alloc(256 * 1024);
    const bytes = fs.readSync(fd, buf, 0, buf.length, 0);
    const text = buf.subarray(0, bytes).toString("utf8");
    let cwd = "";
    let name = "";
    let summary = "";
    let offset = 0;
    while (offset < text.length) {
      const nl = text.indexOf("\n", offset);
      const line = nl === -1 ? text.slice(offset) : text.slice(offset, nl);
      offset = nl === -1 ? text.length : nl + 1;
      if (!line.trim()) continue;
      try {
        const obj = JSON.parse(line);
        if (obj && typeof obj.cwd === "string" && obj.cwd && !cwd) cwd = obj.cwd;
        if (!name) name = pickClaudeSessionName(obj);
        if (!summary) summary = pickClaudeUserSummary(obj);
        if (cwd && name && summary) break;
      } catch {
        /* malformed line — keep looking */
      }
      if (nl === -1) break;
    }
    return { cwd, name, summary };
  } catch {
    return { cwd: "", name: "", summary: "" };
  } finally {
    if (fd !== undefined) {
      try { fs.closeSync(fd); } catch { /* ignore */ }
    }
  }
}

function pickClaudeSessionName(obj) {
  if (!obj || typeof obj !== "object") return "";
  const direct = firstCleanString(
    obj.thread_name,
    obj.threadName,
    obj.session_name,
    obj.sessionName,
    obj.conversation_name,
    obj.conversationName,
    obj.title,
    obj.name,
    obj.summary_title,
    obj.summaryTitle,
    obj.session?.title,
    obj.session?.name,
    obj.conversation?.title,
    obj.conversation?.name,
  );
  if (direct) return direct;

  if (obj.type === "summary") {
    return firstCleanString(obj.summary, obj.message, obj.title, obj.name);
  }
  return "";
}

function pickClaudeUserSummary(obj) {
  if (!obj || typeof obj !== "object") return "";
  if (obj.type === "user" || obj.type === "human") {
    return summarizeUserText(
      pickTextFromContent(obj.message?.content)
      || pickTextFromContent(obj.content)
      || obj.text
      || obj.message,
    );
  }
  const message = obj.message && typeof obj.message === "object" ? obj.message : null;
  if (message && message.role === "user") {
    return summarizeUserText(
      pickTextFromContent(message.content)
      || message.text
      || message.content,
    );
  }
  return "";
}

function pickTextFromContent(content) {
  if (typeof content === "string") return content;
  if (!Array.isArray(content)) return "";
  for (const block of content) {
    if (!block || typeof block !== "object") continue;
    if (typeof block.text === "string" && block.text.trim()) return block.text;
    if (typeof block.content === "string" && block.content.trim()) return block.content;
  }
  return "";
}

function firstCleanString(...values) {
  for (const value of values) {
    if (typeof value !== "string") continue;
    const text = value.trim();
    if (text) return text.slice(0, 80);
  }
  return "";
}

function summarizeUserText(value) {
  const text = typeof value === "string" ? value.trim() : "";
  if (!text) return "";
  if (text.startsWith("# AGENTS.md instructions")) return "";
  if (text.startsWith("<environment_context>")) return "";
  if (text.startsWith("<INSTRUCTIONS>")) return "";
  return text.slice(0, 80);
}

/**
 * Claude Code encodes a cwd into a flat folder name by replacing path
 * separators (and optionally other unsafe chars) with `-`. The encoding
 * isn't published as a stable API; we reverse the *common* case but won't
 * round-trip every cwd. Callers should treat the return value as a *hint*,
 * never as authoritative.
 *
 * @param {string} encoded
 * @returns {string | null}
 */
function decodeProjectDirName(encoded) {
  if (!encoded || typeof encoded !== "string") return null;
  // Common form: `-Users-x-proj` → `/Users/x/proj`. Some versions URL-encode
  // unsafe chars; we don't try to decode those.
  if (!encoded.startsWith("-")) return null;
  return encoded.replace(/-/g, "/");
}

function sameCwd(a, b) {
  try {
    return path.resolve(a) === path.resolve(b);
  } catch {
    return a === b;
  }
}

module.exports = {
  claudeProjectsRoot,
  listClaudeSessions,
  decodeProjectDirName,
};
