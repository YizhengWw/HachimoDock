#!/usr/bin/env node
// Clawd — CodeBuddy hook (stdin JSON with hook_event_name; stdout JSON for gating hooks)
// Registered in ~/.codebuddy/settings.json by hooks/codebuddy-install.js
// CodeBuddy uses Claude Code-compatible hook format with identical event names.

const { postStateToRunningServer, readHostPrefix } = require("./server-config");
const { resolveStableProcessContext } = require("./process-tree");

// CodeBuddy hook event → { state, event } for the Clawd state machine
const HOOK_MAP = {
  SessionStart:     { state: "idle",         event: "SessionStart" },
  SessionEnd:       { state: "sleeping",     event: "SessionEnd" },
  UserPromptSubmit: { state: "working",      event: "UserPromptSubmit" },
  PreToolUse:       { state: "working",      event: "PreToolUse" },
  PostToolUse:      { state: "working",      event: "PostToolUse" },
  Stop:             { state: "attention",    event: "Stop" },
  // PermissionRequest: handled by HTTP hook (blocking), not this command hook
  Notification:     { state: "notification", event: "Notification" },
  PreCompact:       { state: "sweeping",     event: "PreCompact" },
};

let _processContext = null;

function getProcessContext() {
  if (_processContext) return _processContext;
  _processContext = resolveStableProcessContext({
    agentNames: ["codebuddy.exe", "codebuddy"],
    editorMap: {
      "code.exe": "code",
      "cursor.exe": "cursor",
      "codebuddy.exe": "codebuddy",
      code: "code",
      cursor: "cursor",
      "code-insiders": "code",
      codebuddy: "codebuddy",
    },
  });
  return _processContext;
}

// CodeBuddy PreToolUse gating — allow by default
function stdoutForEvent(hookName) {
  if (hookName === "PreToolUse") {
    return JSON.stringify({ decision: "allow" });
  }
  return "{}";
}

// Read stdin JSON, extract event, post state, write stdout
const chunks = [];
let _ran = false;
let _stdinTimer = null;

function finishOnce(payload) {
  if (_ran) return;
  _ran = true;
  if (_stdinTimer) clearTimeout(_stdinTimer);

  const hookName = (payload && payload.hook_event_name) || "";
  const mapped = HOOK_MAP[hookName];

  if (!mapped) {
    process.stdout.write(stdoutForEvent(hookName) + "\n");
    process.exit(0);
    return;
  }

  const { state, event } = mapped;

  if (hookName === "SessionStart" && !process.env.CLAWD_REMOTE) getProcessContext();

  const sessionId = (payload && payload.session_id) || "default";
  const cwd = (payload && payload.cwd) || "";

  const body = { state, session_id: sessionId, event };
  body.agent_id = "codebuddy";
  if (cwd) body.cwd = cwd;
  if (process.env.CLAWD_REMOTE) {
    body.host = readHostPrefix();
  } else {
    const context = getProcessContext();
    body.source_pid = context.stablePid;
    if (context.editor) body.editor = context.editor;
    if (context.agentPid) body.agent_pid = context.agentPid;
    if (context.pidChain.length) body.pid_chain = context.pidChain;
  }

  const outLine = stdoutForEvent(hookName);
  const data = JSON.stringify(body);
  postStateToRunningServer(data, { timeoutMs: 100 }, () => {
    process.stdout.write(outLine + "\n");
    process.exit(0);
  });
}

process.stdin.on("data", (c) => chunks.push(c));
process.stdin.on("end", () => {
  let payload = {};
  try {
    const raw = Buffer.concat(chunks).toString();
    if (raw.trim()) payload = JSON.parse(raw);
  } catch {
    payload = {};
  }
  finishOnce(payload);
});

_stdinTimer = setTimeout(() => finishOnce({}), 400);
