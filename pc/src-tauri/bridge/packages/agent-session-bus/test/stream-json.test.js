"use strict";

const { test } = require("node:test");
const assert = require("node:assert/strict");
const { Readable } = require("node:stream");

const { frameToEvents, parseLine, lineStream } = require("../src/util/stream-json");

test("parseLine handles blank/non-json lines", () => {
  assert.equal(parseLine(""), null);
  assert.equal(parseLine("\r\n"), null);
  assert.equal(parseLine("hello"), null);
  assert.equal(parseLine("{not json"), null);
  assert.deepEqual(parseLine('{"a":1}'), { a: 1 });
  assert.deepEqual(parseLine('  {"a":1}\r'), { a: 1 });
});

test("Claude assistant block with text emits token", () => {
  const events = frameToEvents({
    type: "assistant",
    message: { content: [{ type: "text", text: "好的" }] },
  });
  assert.deepEqual(events, [{ kind: "token", text: "好的" }]);
});

test("Codex assistant message with content array emits tokens", () => {
  const events = frameToEvents({
    type: "message",
    role: "assistant",
    content: [
      { type: "text", text: "Hello " },
      { type: "text", text: "world" },
    ],
  });
  assert.deepEqual(events, [
    { kind: "token", text: "Hello " },
    { kind: "token", text: "world" },
  ]);
});

test("Claude content_block_delta emits a single token", () => {
  const events = frameToEvents({
    type: "content_block_delta",
    delta: { text: "改" },
  });
  assert.deepEqual(events, [{ kind: "token", text: "改" }]);
});

test("Codex agent_turn.delta with top-level text emits a token", () => {
  const events = frameToEvents({
    type: "agent_turn.delta",
    text: "改好了",
  });
  assert.deepEqual(events, [{ kind: "token", text: "改好了" }]);
});

test("tool_use frame emits a tool start", () => {
  const events = frameToEvents({
    type: "tool_use",
    name: "Edit",
    input: { path: "src/main.rs" },
  });
  assert.equal(events.length, 1);
  assert.equal(events[0].kind, "tool");
  assert.equal(events[0].name, "Edit");
  assert.equal(events[0].phase, "start");
  assert.deepEqual(events[0].input, { path: "src/main.rs" });
});

test("tool_result frame emits a tool end with ok=true", () => {
  const events = frameToEvents({ type: "tool_result", name: "Edit" });
  assert.deepEqual(events, [{ kind: "tool", name: "Edit", phase: "end", ok: true }]);
});

test("tool_result frame with is_error emits ok=false", () => {
  const events = frameToEvents({ type: "tool_result", name: "Edit", is_error: true });
  assert.equal(events[0].ok, false);
});

test("Claude result frame emits done with sessionId and tokens", () => {
  const events = frameToEvents({
    type: "result",
    session_id: "abc-123",
    stop_reason: "end_turn",
    usage: { input_tokens: 10, output_tokens: 32 },
  });
  assert.deepEqual(events, [{
    kind: "done",
    sessionId: "abc-123",
    tokens: 42,
    stopReason: "end_turn",
  }]);
});

test("Codex stop frame emits done", () => {
  const events = frameToEvents({
    type: "stop",
    session_id: "codex-xyz",
    reason: "end_turn",
  });
  assert.deepEqual(events, [{
    kind: "done",
    sessionId: "codex-xyz",
    tokens: undefined,
    stopReason: "end_turn",
  }]);
});

test("error frame emits an error event", () => {
  const events = frameToEvents({
    type: "error",
    error: { type: "rate_limit", message: "slow down" },
  });
  assert.deepEqual(events, [{
    kind: "error",
    code: "rate_limit",
    message: "slow down",
  }]);
});

test("unknown frame types yield no events (graceful degradation)", () => {
  const events = frameToEvents({ type: "system", subtype: "init", session_id: "abc" });
  assert.equal(events.length, 0);
});

test("lineStream splits chunks into lines and survives partial chunks", async () => {
  const src = Readable.from([
    "line1\nline2\npart",
    "ial\nline4",
  ], { objectMode: false });
  const lines = [];
  for await (const line of lineStream(src)) lines.push(line);
  assert.deepEqual(lines, ["line1", "line2", "partial", "line4"]);
});
