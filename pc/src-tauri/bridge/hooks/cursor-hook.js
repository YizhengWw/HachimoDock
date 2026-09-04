#!/usr/bin/env node
// Clawd — Cursor Agent hook (stdin JSON, hook_event_name; stdout JSON for gating hooks)
// Registered in ~/.cursor/hooks.json by hooks/cursor-install.js

const { postStateToRunningServer, readHostPrefix } = require("./server-config");
const { resolveStableProcessContext } = require("./process-tree");

const HOOK_TO_STATE = {
  sessionStart: { state: "idle", event: "SessionStart" },
  sessionEnd: { state: "sleeping", event: "SessionEnd" },
  beforeSubmitPrompt: { state: "working", event: "UserPromptSubmit" },
  preToolUse: { state: "working", event: "PreToolUse" },
  postToolUse: { state: "working", event: "PostToolUse" },
  postToolUseFailure: { state: "error", event: "PostToolUseFailure" },
  subagentStart: { state: "juggling", event: "SubagentStart" },
  subagentStop: { state: "working", event: "SubagentStop" },
  preCompact: { state: "sweeping", event: "PreCompact" },
  afterAgentThought: { state: "working", event: "AfterAgentThought" },
};

let _processContext = null;

function getProcessContext() {
  if (_processContext) return _processContext;
  _processContext = resolveStableProcessContext({
    agentNames: ["cursor.exe", "cursor"],
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

function stdoutForCursorHook(hookName) {
  // Only respond with continue for prompt submission; don't override Cursor's permission system
  if (hookName === "beforeSubmitPrompt") return JSON.stringify({ continue: true });
  return "{}";
}

/** Maps Cursor preToolUse/postToolUse tool_name to assets/svg basenames (see state.js DISPLAY_HINT_SVGS). */
function displaySvgFromToolHook(hookName, payload) {
  if (hookName !== "preToolUse" && hookName !== "postToolUse") return undefined;
  const name = payload && payload.tool_name;
  if (!name || typeof name !== "string") return undefined;
  if (name === "Shell" || name.startsWith("MCP:")) return "clawd-working-building.svg";
  if (name === "Task") return "clawd-working-juggling.svg";
  if (name === "Write" || name === "Delete") return "clawd-working-typing.svg";
  if (name === "Read" || name === "Grep") return "clawd-idle-reading.svg";
  return undefined;
}

function resolveStateAndEvent(payload, hookName) {
  if (!hookName) return null;
  if (hookName === "stop") {
    const st = payload && payload.status;
    if (st === "error") return { state: "error", event: "StopFailure" };
    return { state: "attention", event: "Stop" };
  }
  return HOOK_TO_STATE[hookName] || null;
}

function runWithPayload(payload) {
  const argvOverride = process.argv[2];
  const hookNameResolved = argvOverride || (payload && payload.hook_event_name) || "";
  const mapped = resolveStateAndEvent(payload, hookNameResolved);
  if (!mapped) {
    process.stdout.write(stdoutForCursorHook(hookNameResolved) + "\n");
    process.exit(0);
    return;
  }

  const { state, event } = mapped;
  if (hookNameResolved === "sessionStart" && !process.env.CLAWD_REMOTE) getProcessContext();

  const sessionId =
    (payload && (payload.conversation_id || payload.session_id)) || "default";
  let cwd = (payload && payload.cwd) || "";
  if (!cwd && payload && Array.isArray(payload.workspace_roots) && payload.workspace_roots[0]) {
    cwd = payload.workspace_roots[0];
  }

  const body = { state, session_id: sessionId, event };
  body.agent_id = "cursor-agent";
  const hint = displaySvgFromToolHook(hookNameResolved, payload);
  if (hint !== undefined) body.display_svg = hint;
  if (cwd) body.cwd = cwd;
  if (process.env.CLAWD_REMOTE) {
    body.host = readHostPrefix();
  } else {
    const context = getProcessContext();
    body.source_pid = context.stablePid;
    body.editor = context.editor || "cursor";
    if (context.agentPid) {
      body.agent_pid = context.agentPid;
      body.cursor_pid = context.agentPid;
    }
    if (context.pidChain.length) body.pid_chain = context.pidChain;
  }

  const outLine = stdoutForCursorHook(hookNameResolved);
  const data = JSON.stringify(body);
  postStateToRunningServer(data, { timeoutMs: 100 }, () => {
    process.stdout.write(outLine + "\n");
    process.exit(0);
  });
}

let _ran = false;
let _stdinTimer = null;
function finishOnce(payload) {
  if (_ran) return;
  _ran = true;
  if (_stdinTimer) clearTimeout(_stdinTimer);
  runWithPayload(payload || {});
}

const chunks = [];
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
