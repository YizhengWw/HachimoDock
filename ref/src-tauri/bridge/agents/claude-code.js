"use strict";

function readPositiveNumberEnv(name, fallback) {
  const raw = Number(process.env[name]);
  return Number.isFinite(raw) && raw > 0 ? raw : fallback;
}

module.exports = {
  PROJECTS_ROOT: process.env.CLAWD_CLAUDE_PROJECTS_ROOT || "",
  POLL_INTERVAL_MS: readPositiveNumberEnv("CLAWD_CLAUDE_POLL_INTERVAL_MS", 1000),
  HEARTBEAT_MS: 30000,
  DEFAULT_SESSION_ID: "claude:local",
  NEW_FILE_MAX_AGE_MS: 120000,
  INITIAL_TAIL_BYTES: 1048576,
  MAX_SCAN_FILES: 200,
  PROCESS_NAMES_WIN: ["claude.exe", "claude.cmd", "claude"],
  PROCESS_NAMES_UNIX: ["claude"],
};
