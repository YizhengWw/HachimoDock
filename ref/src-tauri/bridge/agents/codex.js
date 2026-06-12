"use strict";

/*
 * [Input] Codex CLI/Desktop JSONL rollout logs, optionally pinned by CLAWD_CODEX_SESSION_DIR.
 * [Output] Polling defaults consumed by codex-log-monitor.
 * [Pos] Agent-specific monitor config for the headless bridge.
 * [Sync] If Codex log discovery changes, update `ref/.folder.md`.
 */

const os = require("os");
const path = require("path");

function envPath(name) {
  const value = process.env[name];
  return typeof value === "string" && value.trim().length > 0 ? value.trim() : "";
}

function resolveCodexHome() {
  const codexHome = envPath("CLAWD_CODEX_HOME") || envPath("CODEX_HOME");
  if (codexHome) return codexHome;
  return path.join(os.homedir(), ".codex");
}

function resolveSessionDir() {
  const explicitSessionDir = envPath("CLAWD_CODEX_SESSION_DIR") || envPath("CODEX_SESSION_DIR");
  if (explicitSessionDir) return explicitSessionDir;

  return path.join(resolveCodexHome(), "sessions");
}

function resolveSessionIndexPath() {
  const explicitIndexPath = envPath("CLAWD_CODEX_SESSION_INDEX_PATH") || envPath("CODEX_SESSION_INDEX_PATH");
  if (explicitIndexPath) return explicitIndexPath;

  return path.join(resolveCodexHome(), "session_index.jsonl");
}

module.exports = {
  SESSION_DIR: resolveSessionDir(),
  SESSION_INDEX_PATH: resolveSessionIndexPath(),
  POLL_INTERVAL_MS: 1500,
  STALE_TIMEOUT_MS: 300000,
  NEW_FILE_MAX_AGE_MS: 120000,
  INITIAL_TAIL_BYTES: 1048576,
  // Codex desktop can keep appending to an older rollout file for days.
  // Keep a wider scan window so long-running threads are still detected.
  LOOKBACK_DAYS: 30,
  LOG_EVENT_MAP: {
    session_meta: "idle",
    "event_msg:task_started": "thinking",
    "event_msg:user_message": "thinking",
    "event_msg:agent_message": "speaking",
    "response_item:function_call": "working",
    "response_item:custom_tool_call": "working",
    "response_item:web_search_call": "working",
    "event_msg:task_complete": "attention",
    "event_msg:context_compacted": "sweeping",
    "event_msg:turn_aborted": "idle",
  },
};
