"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { createHash } = require("node:crypto");
const { readCompletionReceipt } = require("../hooks/workbuddy-display");
const { stateForHook } = require("../hooks/workbuddy-pet-manager-hook");
const { SessionMetricsTracker } = require("../packages/clawd-backend-service/src/status-metrics");
const hash = s => createHash("sha256").update(s).digest("hex");
function fixture(t) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "pet-buddy-receipt-"));
  t.after(() => fs.rmSync(home, { recursive: true, force: true }));
  const dir = path.join(home, ".workbuddy", "projects", "test");
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, "session.jsonl");
  const timestamp = Date.now() - 1000;
  const rows = [
    { id: "u", type: "message", role: "user", content: [{ type: "input_text", text: "PRIVATE_REQUEST" }] },
    { id: "r", parentId: "u", type: "reasoning", content: "PRIVATE_REASONING" },
    { id: "f", parentId: "r", type: "function_call", arguments: "PRIVATE_ARGUMENTS" },
    { id: "o", parentId: "f", type: "function_call_result", output: "PRIVATE_OUTPUT" },
    { id: "a", parentId: "o", type: "message", role: "assistant", status: "completed",
      providerData: { conversationRequestId: "model-request" }, content: [{ type: "output_text", text: "本轮完成" }] },
  ].map((r, i) => ({ sessionId: "session", timestamp: timestamp + i, ...r }));
  const options = { home, env: {} };
  const payload = { hook_event_name: "Stop", session_id: "session", transcript_path: file,
    generation_id: "different-hook-generation", last_assistant_message: "本轮完成" };
  const write = data => fs.writeFileSync(file, data.map(JSON.stringify).join("\n") + "\n");
  write(rows);
  return { rows, write, options, payload, timestamp };
}

test("WorkBuddy Stop resolves an exact parent chain when submission and tool generations differ", t => {
  const f = fixture(t);
  const receipt = readCompletionReceipt(f.payload, f.options);
  assert.equal(receipt.userHash, hash("PRIVATE_REQUEST"));
  assert.equal(receipt.userTimestamp, f.timestamp);
  assert.equal(receipt.receiptSource, "transcript_chain");
  assert.equal(receipt.reply, "本轮完成");
  assert.equal(readCompletionReceipt({ ...f.payload, last_assistant_message: undefined,
    generation_id: "model-request" }, f.options).userHash, receipt.userHash);
  assert.equal(readCompletionReceipt({ ...f.payload, last_assistant_message: "另一轮完成" }, f.options), null);
  const tracker = new SessionMetricsTracker();
  const baseline = tracker.sessionEvents("workbuddy");
  for (const input of [{ hook_event_name: "UserPromptSubmit", session_id: "session", prompt: "PRIVATE_REQUEST" }, f.payload]) {
    const hook = stateForHook(input, f.options);
    tracker.apply({ source: "workbuddy", sessionId: hook.session_id, event: hook.event, state: hook.state,
      taskCorrelation: hook.task_correlation, display: { content: hook.display_content } });
  }
  const events = tracker.sessionEvents("workbuddy", baseline).events;
  assert.equal(events[0].taskCorrelation.turnId, "");
  assert.equal(events[1].taskCorrelation.userHash, hash("PRIVATE_REQUEST"));
  assert.equal(events[1].taskCorrelation.receiptSource, "transcript_chain");
  assert.equal(events[1].taskCorrelation.userTimestamp, f.timestamp);
  assert.doesNotMatch(JSON.stringify(events[1]), /PRIVATE_|transcript_path|different-hook-generation/);
});

test("WorkBuddy rejects broken, cyclic, duplicate, foreign and truncated ancestry", t => {
  const f = fixture(t);
  const variants = [
    f.rows.filter(r => r.id !== "u"),
    f.rows.map(r => r.id === "o" ? { ...r, parentId: "a" } : r),
    [...f.rows, f.rows[0]],
    f.rows.map(r => r.id === "r" ? { ...r, sessionId: "other" } : r),
    f.rows.map(r => r.id === "u" ? { ...r, timestamp: Date.now() + 10000 } : r),
    f.rows.map(r => r.id === "u" ? { ...r, content: [{ type: "tool_result", text: "PRIVATE_REQUEST" }] } : r),
  ];
  for (const rows of variants) {
    f.write(rows);
    assert.equal(readCompletionReceipt(f.payload, f.options), null);
  }
  f.write(f.rows);
  fs.writeFileSync(f.payload.transcript_path, JSON.stringify(f.rows[0]) + "\n" + "x".repeat(300000) + "\n" + f.rows.slice(1).map(JSON.stringify).join("\n"));
  assert.equal(readCompletionReceipt(f.payload, f.options), null);
});

test("overlapping WorkBuddy replies follow their own parents, not the latest user", t => {
  const f = fixture(t);
  const second = f.rows.map(r => ({ ...r, id: r.id + "2", parentId: r.parentId ? r.parentId + "2" : undefined,
    content: r.role === "user" ? [{ type: "input_text", text: "SECOND_REQUEST" }] : r.role === "assistant" ? [{ type: "output_text", text: "第二轮完成" }] : r.content,
    providerData: r.role === "assistant" ? { conversationRequestId: "second-model" } : r.providerData }));
  f.write([...f.rows.slice(0, 4), ...second, f.rows[4]]);
  assert.equal(readCompletionReceipt(f.payload, f.options).userHash, hash("PRIVATE_REQUEST"));
  assert.equal(readCompletionReceipt({ ...f.payload, generation_id: "second-model", last_assistant_message: "第二轮完成" }, f.options).userHash, hash("SECOND_REQUEST"));
  // Repeated identical replies without a uniquely matching generation are ambiguous.
  second[4].content = f.rows[4].content;
  f.write([...f.rows, ...second]);
  assert.equal(readCompletionReceipt(f.payload, f.options), null);
  assert.equal(readCompletionReceipt({ ...f.payload, generation_id: "second-model" }, f.options).userHash, hash("SECOND_REQUEST"));
});

test("receipt proof does not cross path boundaries, sessions or nonterminal hooks", t => {
  const f = fixture(t);
  for (const override of [{ session_id: "other" }, { hook_event_name: "PreToolUse" }, { transcript_path: "/not/a/transcript" }]) {
    assert.equal(readCompletionReceipt({ ...f.payload, ...override }, f.options), null);
  }
  const tracker = new SessionMetricsTracker(); const baseline = tracker.sessionEvents("workbuddy");
  tracker.apply({ source: "workbuddy", sessionId: "session", event: "PreToolUse", state: "working",
    taskCorrelation: { userHash: hash("PRIVATE_REQUEST"), turnId: "g", receiptSource: "transcript_chain", userTimestamp: f.timestamp } });
  const c = tracker.sessionEvents("workbuddy", baseline).events[0].taskCorrelation;
  assert.equal(c.userHash, "");
  assert.equal(c.receiptSource, undefined);
});
