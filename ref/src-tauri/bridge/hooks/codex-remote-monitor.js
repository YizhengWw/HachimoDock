#!/usr/bin/env node
// Codex CLI JSONL log monitor — standalone remote version
// Polls ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl for state changes
// and POSTs them via HTTP to the local Clawd desktop pet (through SSH tunnel).
//
// Zero external dependencies — Node.js built-ins + ./server-config.js only.
//
// Usage:
//   node codex-remote-monitor.js            # run as long-lived daemon
//   node codex-remote-monitor.js --once     # single scan then exit (debug)
//   node codex-remote-monitor.js --port 23334  # custom server port
//
// Designed to keep running even when the SSH tunnel is down — failed POSTs
// are silently ignored, and the monitor resumes syncing as soon as the
// tunnel comes back up.

const fs = require("fs");
const path = require("path");
const os = require("os");
const { postStateToRunningServer, readHostPrefix } = require("./server-config");

// ── Inline config from agents/codex.js (zero-dependency requirement) ──

const SESSION_DIR = path.join(os.homedir(), ".codex", "sessions");
const POLL_INTERVAL_MS = 1500;

// JSONL record type[:subtype] → pet state
// ⚠️ Duplicated from agents/codex.js logEventMap (zero-dep requirement) — keep in sync
const LOG_EVENT_MAP = {
  "session_meta": "idle",
  "event_msg:task_started": "working",
  "event_msg:user_message": "working",
  "event_msg:agent_message": "speaking",
  "response_item:function_call": "working",
  "response_item:custom_tool_call": "working",
  "response_item:web_search_call": "working",
  "event_msg:task_complete": "attention",
  "event_msg:context_compacted": "sweeping",
  "event_msg:turn_aborted": "idle",
};

// ── CLI args ──

const args = process.argv.slice(2);
const onceMode = args.includes("--once");
const portIndex = args.indexOf("--port");
const preferredPort = portIndex >= 0 ? parseInt(args[portIndex + 1], 10) : undefined;

const hostPrefix = readHostPrefix();

// ── State tracking ──

// Map<filePath, { offset, sessionId, cwd, lastEventTime, lastState, partial }>
const tracked = new Map();

// ── Core polling logic (mirrors agents/codex-log-monitor.js) ──

function getSessionDirs() {
  const dirs = [];
  const now = new Date();
  for (let daysAgo = 0; daysAgo <= 1; daysAgo++) {
    const d = new Date(now);
    d.setDate(d.getDate() - daysAgo);
    const yyyy = d.getFullYear();
    const mm = String(d.getMonth() + 1).padStart(2, "0");
    const dd = String(d.getDate()).padStart(2, "0");
    dirs.push(path.join(SESSION_DIR, String(yyyy), mm, dd));
  }
  return dirs;
}

function extractSessionId(fileName) {
  // rollout-2026-03-25T15-10-51-019d23d4-f1a9-7633-b9c7-758327137228.jsonl
  const base = fileName.replace(".jsonl", "");
  const parts = base.split("-");
  if (parts.length < 10) return null;
  return parts.slice(-5).join("-");
}

function readNumber(value) {
  return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

function extractTokenUsage(payload) {
  if (!payload || typeof payload !== "object" || payload.type !== "token_count") return null;
  const info = payload.info && typeof payload.info === "object" ? payload.info : null;
  const total = info && info.total_token_usage && typeof info.total_token_usage === "object"
    ? info.total_token_usage
    : null;
  const last = info && info.last_token_usage && typeof info.last_token_usage === "object"
    ? info.last_token_usage
    : null;
  const out = {
    inputTokens: readNumber(total && total.input_tokens),
    outputTokens: readNumber(total && total.output_tokens),
    cachedInputTokens: readNumber(total && total.cached_input_tokens),
    reasoningOutputTokens: readNumber(total && total.reasoning_output_tokens),
    totalTokens: readNumber(total && total.total_tokens),
    lastInputTokens: readNumber(last && last.input_tokens),
    lastOutputTokens: readNumber(last && last.output_tokens),
    lastCachedInputTokens: readNumber(last && last.cached_input_tokens),
    lastReasoningOutputTokens: readNumber(last && last.reasoning_output_tokens),
    lastTotalTokens: readNumber(last && last.total_tokens),
    modelContextWindow: readNumber(info && info.model_context_window),
  };
  const hasAny = Object.values(out).some((value) => Number.isFinite(value));
  return hasAny ? out : null;
}

function postState(sessionId, state, event, cwd, tokenUsage) {
  const body = JSON.stringify({
    state,
    session_id: sessionId,
    event,
    agent_id: "codex",
    cwd: cwd || "",
    token_usage: tokenUsage || undefined,
    host: hostPrefix,
  });
  postStateToRunningServer(
    body,
    { timeoutMs: 100, preferredPort },
    () => {} // fire and forget — tunnel may be down
  );
}

function processLine(line, entry) {
  let obj;
  try {
    obj = JSON.parse(line);
  } catch {
    return;
  }

  const type = obj.type;
  const payload = obj.payload;
  const subtype =
    payload && typeof payload === "object" ? payload.type || "" : "";
  const key = subtype ? type + ":" + subtype : type;

  // Extract CWD from session_meta
  if (type === "session_meta" && payload) {
    entry.cwd = payload.cwd || "";
  }

  if (key === "event_msg:token_count") {
    const tokenUsage = extractTokenUsage(payload);
    if (!tokenUsage) return;
    entry.tokenUsage = tokenUsage;
    entry.lastEventTime = Date.now();
    postState(entry.sessionId, entry.lastState || "idle", key, entry.cwd, entry.tokenUsage);
    return;
  }

  const state = LOG_EVENT_MAP[key];
  if (state === undefined || state === null) return;

  // Avoid spamming same state
  if (state === entry.lastState && (state === "working" || state === "speaking")) return;
  entry.lastState = state;
  entry.lastEventTime = Date.now();

  postState(entry.sessionId, state, key, entry.cwd, entry.tokenUsage);
}

function pollFile(filePath, fileName) {
  let stat;
  try {
    stat = fs.statSync(filePath);
  } catch {
    return;
  }

  let entry = tracked.get(filePath);
  if (!entry) {
    const sessionId = extractSessionId(fileName);
    if (!sessionId) return;
    entry = {
      offset: 0,
      sessionId: "codex:" + sessionId,
      cwd: "",
      lastEventTime: Date.now(),
      lastState: null,
      partial: "",
      tokenUsage: null,
    };
    tracked.set(filePath, entry);
  }

  if (stat.size <= entry.offset) return;

  let buf;
  try {
    const fd = fs.openSync(filePath, "r");
    const readLen = stat.size - entry.offset;
    buf = Buffer.alloc(readLen);
    fs.readSync(fd, buf, 0, readLen, entry.offset);
    fs.closeSync(fd);
  } catch {
    return;
  }
  entry.offset = stat.size;

  const text = entry.partial + buf.toString("utf8");
  const lines = text.split("\n");
  entry.partial = lines.pop() || "";

  for (const line of lines) {
    if (!line.trim()) continue;
    processLine(line, entry);
  }
}

function cleanStaleFiles() {
  const now = Date.now();
  for (const [filePath, entry] of tracked) {
    if (now - entry.lastEventTime > 300000) {
      postState(entry.sessionId, "sleeping", "stale-cleanup", entry.cwd, entry.tokenUsage);
      tracked.delete(filePath);
    }
  }
}

function poll() {
  const dirs = getSessionDirs();
  for (const dir of dirs) {
    let files;
    try {
      files = fs.readdirSync(dir);
    } catch {
      continue;
    }
    const now = Date.now();
    for (const file of files) {
      if (!file.startsWith("rollout-") || !file.endsWith(".jsonl")) continue;
      const filePath = path.join(dir, file);
      if (!tracked.has(filePath)) {
        try {
          const mtime = fs.statSync(filePath).mtimeMs;
          if (now - mtime > 120000) continue;
        } catch { continue; }
      }
      pollFile(filePath, file);
    }
  }
  cleanStaleFiles();
}

// ── Main ──

console.log(`Clawd Codex remote monitor started`);
console.log(`  Session dir: ${SESSION_DIR}`);
console.log(`  Poll interval: ${POLL_INTERVAL_MS}ms`);
if (preferredPort) console.log(`  Preferred port: ${preferredPort}`);
console.log(`  Press Ctrl+C to stop\n`);

poll();

if (!onceMode) {
  const interval = setInterval(poll, POLL_INTERVAL_MS);

  process.on("SIGINT", () => {
    clearInterval(interval);
    console.log("\nStopped.");
    process.exit(0);
  });
  process.on("SIGTERM", () => {
    clearInterval(interval);
    process.exit(0);
  });
}
