#!/usr/bin/env node
// Clawd Desktop Pet — Claude Code Hook Script
// Zero dependencies, fast cold start, 1s timeout
// Usage: node clawd-hook.js <event_name>
// Reads stdin JSON from Claude Code for session_id

const { postStateToRunningServer, readHostPrefix } = require("./server-config");
const { resolveStableProcessContext } = require("./process-tree");

const EVENT_TO_STATE = {
  SessionStart: "idle",
  SessionEnd: "sleeping",
  UserPromptSubmit: "working",
  AssistantMessage: "speaking",
  AssistantOutput: "speaking",
  PreToolUse: "working",
  PostToolUse: "working",
  PostToolUseFailure: "error",
  Stop: "attention",
  StopFailure: "error",
  SubagentStart: "juggling",
  SubagentStop: "working",
  PreCompact: "sweeping",
  PostCompact: "attention",
  Notification: "notification",
  // PermissionRequest is handled by HTTP hook (blocking) — not command hook
  Elicitation: "notification",
  WorktreeCreate: "carrying",
};

const event = process.argv[2];
const state = EVENT_TO_STATE[event];
if (!state) process.exit(0);

let _processContext = null;

function getProcessContext() {
  if (_processContext) return _processContext;
  _processContext = resolveStableProcessContext({
    agentNames: ["claude.exe", "claude"],
    agentCommandMarkers: ["claude-code", "@anthropic-ai"],
    headlessPattern: /\s(-p|--print)(\s|$)/,
    editorMap: {
      "code.exe": "code",
      "cursor.exe": "cursor",
      code: "code",
      cursor: "cursor",
      "code-insiders": "code",
    },
  });
  return _processContext;
}

// Pre-resolve on SessionStart (runs during stdin buffering, not after)
// Remote mode: skip PID collection — remote PIDs are meaningless on the local machine
// and could collide with local PIDs, confusing the process-alive checks in state.js.
if (event === "SessionStart" && !process.env.CLAWD_REMOTE) getProcessContext();

// Read stdin for session_id (Claude Code pipes JSON with session metadata)
const chunks = [];
let sent = false;

process.stdin.on("data", (c) => chunks.push(c));
process.stdin.on("end", () => {
  let sessionId = "default";
  let cwd = "";
  let source = "";
  try {
    const payload = JSON.parse(Buffer.concat(chunks).toString());
    sessionId = payload.session_id || "default";
    cwd = payload.cwd || "";
    source = payload.source || payload.reason || "";
  } catch {}
  send(sessionId, cwd, source);
});

// Safety: if stdin doesn't end in 400ms, send with default session
// (200ms was too aggressive on slow machines / AV scanning)
setTimeout(() => send("default", ""), 400);

function send(sessionId, cwd, source) {
  if (sent) return;
  sent = true;

  // /clear triggers SessionEnd → SessionStart in quick succession;
  // show sweeping (clearing context) instead of sleeping
  const resolvedState = (event === "SessionEnd" && source === "clear") ? "sweeping" : state;

  const body = { state: resolvedState, session_id: sessionId, event };
  body.agent_id = "claude-code";
  if (cwd) body.cwd = cwd;
  if (process.env.CLAWD_REMOTE) {
    body.host = readHostPrefix();
  } else {
    const context = getProcessContext();
    // Walk to stable terminal PID — process.ppid is an ephemeral shell
    // that dies when the hook exits, so it's useless for later focus calls
    body.source_pid = context.stablePid;
    if (context.editor) body.editor = context.editor;
    if (context.agentPid) {
      body.agent_pid = context.agentPid;
      body.claude_pid = context.agentPid; // backward compat with older Clawd versions
    }
    if (context.pidChain.length) body.pid_chain = context.pidChain;
    if (context.isHeadless) body.headless = true;
  }

  const data = JSON.stringify(body);
  postStateToRunningServer(
    data,
    { timeoutMs: 100 }, // runtime port first, then a small local fallback range
    () => process.exit(0)
  );
}
