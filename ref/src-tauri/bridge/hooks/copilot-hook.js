#!/usr/bin/env node
// Clawd Desktop Pet — Copilot CLI Hook Script
// Zero dependencies, fast cold start, 1s timeout
// Usage: node copilot-hook.js <event_name>
// Reads stdin JSON from Copilot CLI for sessionId (camelCase)

const { postStateToRunningServer } = require("./server-config");
const { resolveStableProcessContext } = require("./process-tree");

const EVENT_TO_STATE = {
  sessionStart: "idle",
  sessionEnd: "sleeping",
  userPromptSubmitted: "working",
  preToolUse: "working",
  postToolUse: "working",
  errorOccurred: "error",
  agentStop: "attention",
  subagentStart: "juggling",
  subagentStop: "working",
  preCompact: "sweeping",
};

const event = process.argv[2];
const state = EVENT_TO_STATE[event];
if (!state) process.exit(0);

let _processContext = null;

function getProcessContext() {
  if (_processContext) return _processContext;
  _processContext = resolveStableProcessContext({
    agentNames: ["copilot.exe", "copilot"],
    agentCommandMarkers: ["@github/copilot"],
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

// Pre-resolve on sessionStart
if (event === "sessionStart") getProcessContext();

// Read stdin for sessionId (Copilot CLI uses camelCase field names)
const chunks = [];
let sent = false;

process.stdin.on("data", (c) => chunks.push(c));
process.stdin.on("end", () => {
  let sessionId = "default";
  let cwd = "";
  try {
    const payload = JSON.parse(Buffer.concat(chunks).toString());
    // Copilot CLI uses camelCase: sessionId, not session_id
    sessionId = payload.sessionId || payload.session_id || "default";
    cwd = payload.cwd || "";
  } catch {}
  send(sessionId, cwd);
});

setTimeout(() => send("default", ""), 400);

function send(sessionId, cwd) {
  if (sent) return;
  sent = true;

  const body = { state, session_id: sessionId, event };
  body.agent_id = "copilot-cli";
  if (cwd) body.cwd = cwd;
  const context = getProcessContext();
  body.source_pid = context.stablePid;
  if (context.editor) body.editor = context.editor;
  if (context.agentPid) body.agent_pid = context.agentPid;
  if (context.pidChain.length) body.pid_chain = context.pidChain;

  const data = JSON.stringify(body);
  postStateToRunningServer(
    data,
    { timeoutMs: 100 },
    () => process.exit(0)
  );
}
