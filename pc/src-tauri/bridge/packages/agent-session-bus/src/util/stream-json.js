"use strict";

/**
 * Pure-function parser for Claude Code / Codex line-delimited JSON event
 * streams.
 *
 * The two CLIs emit superficially similar but mutually incompatible shapes
 * which have also drifted across versions. We commit to the **minimum
 * stable surface** that's been steady across the window we care about:
 *
 *  Claude (`--output-format stream-json`):
 *    - "assistant" / "message" / "delta" text fragments → token
 *    - tool_use / tool_result lifecycle              → tool start/end
 *    - "result" with is_error                        → error
 *    - "result" / "stop"                             → done
 *
 *  Codex (`--json`, 0.118+):
 *    - "thread.started"                              → ignored at the event
 *                                                       layer; the adapter
 *                                                       picks `thread_id`
 *                                                       up out-of-band as
 *                                                       the session id.
 *    - "turn.started"                                → ignored
 *    - "item.completed" with item.type=agent_message → token (whole text)
 *    - "item.completed" with item.type=reasoning     → ignored (don't feed
 *                                                       chain-of-thought
 *                                                       to TTS)
 *    - "turn.completed" with usage                   → done
 *    - "turn.failed" / "thread.failed" / type=error  → error
 *
 * Anything we don't recognise we drop on the floor — we never crash the
 * stream because the CLI introduced a new event kind.
 *
 * The emitted shape is the same `AgentEvent` enum the bus speaks
 * (see adapters/base.js).
 *
 * @typedef {{kind:"token", text:string} |
 *          {kind:"tool", name:string, phase:"start"|"end", input?:any, ok?:boolean} |
 *          {kind:"done", sessionId:string, tokens?:number, stopReason?:string} |
 *          {kind:"error", code:string, message:string}} AgentEvent
 */

/**
 * Convert a single parsed stream-json frame to zero or more AgentEvents.
 * We return an array so a single frame containing multiple content blocks
 * (e.g. `message_delta` with several text deltas) maps cleanly.
 *
 * @param {object} frame
 * @returns {AgentEvent[]}
 */
function frameToEvents(frame) {
  if (!frame || typeof frame !== "object") return [];
  const out = [];

  const type = typeof frame.type === "string" ? frame.type : "";

  // Claude: { type: "assistant", message: { content: [...] } }
  // Codex:  { type: "message", role: "assistant", content: [...] }
  if (
    (type === "assistant" || type === "message") &&
    (frame.message?.content || frame.content)
  ) {
    const content = frame.message?.content || frame.content;
    if (Array.isArray(content)) {
      for (const block of content) collectFromContentBlock(block, out);
    }
  }

  // Claude streaming delta: { type: "content_block_delta", delta: { text } }
  if (type === "content_block_delta" && frame.delta) {
    const text = pickText(frame.delta);
    if (text) out.push({ kind: "token", text });
  }

  // Codex streaming delta: { type: "delta", delta: { text } } / similar
  if (type === "delta" && frame.delta) {
    const text = pickText(frame.delta);
    if (text) out.push({ kind: "token", text });
  }

  // Codex: { type: "agent_turn.delta", text: "..." }
  if (type.endsWith(".delta") && typeof frame.text === "string") {
    out.push({ kind: "token", text: frame.text });
  }

  // Codex 0.118+ "item.completed":
  //   { type: "item.completed",
  //     item: { id, type: "agent_message" | "reasoning" | "command_*",
  //             text: "..." | summary: "..." } }
  //
  // The agent_message item carries the full assistant turn as a single
  // string (codex doesn't deltafy through --json). We emit it as one
  // token event; downstream TTS will speak it once. Reasoning frames are
  // chain-of-thought and must NOT be exposed to TTS or chat history.
  if (type === "item.completed" && frame.item && typeof frame.item === "object") {
    const item = frame.item;
    const itemType = typeof item.type === "string" ? item.type : "";
    if (itemType === "agent_message" && typeof item.text === "string" && item.text) {
      out.push({ kind: "token", text: item.text });
    } else if (itemType === "tool_call" || itemType === "command_started") {
      out.push({
        kind: "tool",
        name: pickToolName(item),
        phase: "start",
        input: item.input ?? item.command ?? item.arguments,
      });
    } else if (itemType === "tool_call_output" || itemType === "command_completed") {
      out.push({
        kind: "tool",
        name: pickToolName(item),
        phase: "end",
        ok: !item.is_error && !item.error,
      });
    }
    // reasoning, model_thoughts, etc. — intentionally dropped.
  }

  // Codex 0.118+ explicit incremental delta on agent_message (not yet
  // emitted by 0.125 with current providers, but documented in the schema
  // and used by some OSS providers). Mirror the agent_message handling.
  if (type === "item.delta" && frame.item && typeof frame.item === "object") {
    const item = frame.item;
    if (item.type === "agent_message" && typeof item.text === "string" && item.text) {
      out.push({ kind: "token", text: item.text });
    }
  }

  // Tool use start: { type: "tool_use" } or { type: "tool_call" }
  if (type === "tool_use" || type === "tool_call") {
    out.push({
      kind: "tool",
      name: pickToolName(frame),
      phase: "start",
      input: frame.input ?? frame.arguments,
    });
  }

  // Tool result: { type: "tool_result" } / { type: "tool_call_output" }
  if (type === "tool_result" || type === "tool_call_output") {
    out.push({
      kind: "tool",
      name: pickToolName(frame),
      phase: "end",
      ok: !frame.is_error && !frame.error,
    });
  }

  // Claude: { type: "result", session_id, usage, stop_reason }
  // Codex (legacy):  { type: "stop", session_id, ... }
  // Some Codex builds use { type: "agent_turn.end" }
  // Codex 0.118+:    { type: "turn.completed", usage: {...} }
  if (
    type === "result" ||
    type === "stop" ||
    type === "agent_turn.end" ||
    type === "turn.completed"
  ) {
    // Distinguish "ended with an error" from "ended normally". Claude
    // signals failure with `is_error: true` (sometimes alongside an
    // `errors` array, e.g. `errors: ["No conversation found with session
    // ID: <sid>"]`). If we mapped that to a plain `done`, the caller
    // would see a successful turn that produced zero tokens — which is
    // exactly what voice-service used to do, leaving the user with
    // silence for an unrelated reason. Surface the actual error so the
    // bus can emit a proper `error` SSE event.
    if (frame.is_error === true || (Array.isArray(frame.errors) && frame.errors.length > 0)) {
      const subtype = typeof frame.subtype === "string" ? frame.subtype : "";
      const firstError = Array.isArray(frame.errors) && frame.errors.length > 0
        ? String(frame.errors[0])
        : "";
      out.push({
        kind: "error",
        code: subtype || "AGENT_RESULT_ERROR",
        message: firstError || pickErrorMessage(frame) || "agent ended with error",
      });
    } else {
      // Default stopReason: turn.completed is semantically "end_turn"
      // even when codex omits the explicit field, which matches the
      // behaviour callers used to get from Claude's `result` frames.
      const defaultReason = type === "turn.completed" ? "end_turn" : undefined;
      out.push({
        kind: "done",
        sessionId: pickSessionId(frame),
        tokens: pickTokens(frame),
        stopReason: typeof frame.stop_reason === "string"
          ? frame.stop_reason
          : (typeof frame.reason === "string" ? frame.reason : defaultReason),
      });
    }
  }

  // Some Claude versions emit a top-level { type:"system", subtype:"init", session_id } at start.
  // We don't emit a public event for it but the caller can still use the parsed frame
  // (returned via parseLine) to learn the session id before any tokens arrive.

  // Errors: { type: "error", error: { type, message } } or { error: ... }
  // Codex 0.118+ also emits { type: "turn.failed" } / { type: "thread.failed" }
  // with a top-level `error: {...}` block.
  if (
    type === "error" ||
    type === "turn.failed" ||
    type === "thread.failed" ||
    frame.error
  ) {
    out.push({
      kind: "error",
      code: pickErrorCode(frame),
      message: pickErrorMessage(frame),
    });
  }

  return out;
}

function collectFromContentBlock(block, out) {
  if (!block || typeof block !== "object") return;
  const t = typeof block.type === "string" ? block.type : "";
  if (t === "text" && typeof block.text === "string" && block.text) {
    out.push({ kind: "token", text: block.text });
    return;
  }
  if (t === "tool_use") {
    out.push({
      kind: "tool",
      name: pickToolName(block),
      phase: "start",
      input: block.input,
    });
    return;
  }
  if (t === "tool_result") {
    out.push({
      kind: "tool",
      name: pickToolName(block),
      phase: "end",
      ok: !block.is_error && !block.error,
    });
  }
}

function pickText(delta) {
  if (!delta || typeof delta !== "object") return "";
  if (typeof delta.text === "string") return delta.text;
  if (typeof delta.content === "string") return delta.content;
  return "";
}

function pickToolName(frame) {
  if (!frame || typeof frame !== "object") return "unknown";
  if (typeof frame.name === "string" && frame.name) return frame.name;
  if (typeof frame.tool === "string" && frame.tool) return frame.tool;
  if (typeof frame.tool_name === "string" && frame.tool_name) return frame.tool_name;
  return "unknown";
}

function pickSessionId(frame) {
  if (!frame || typeof frame !== "object") return "";
  return typeof frame.session_id === "string" ? frame.session_id
    : typeof frame.sessionId === "string" ? frame.sessionId
    // Codex 0.118+ session id arrives as `thread_id` on the
    // `thread.started` frame (and on subsequent rollout frames).
    : typeof frame.thread_id === "string" ? frame.thread_id
    : typeof frame.threadId === "string" ? frame.threadId
    : "";
}

function pickTokens(frame) {
  const usage = frame?.usage || frame?.message?.usage;
  if (!usage || typeof usage !== "object") return undefined;
  const total =
    (Number.isFinite(usage.output_tokens) ? usage.output_tokens : 0) +
    (Number.isFinite(usage.input_tokens) ? usage.input_tokens : 0);
  return total > 0 ? total : undefined;
}

function pickErrorCode(frame) {
  if (typeof frame?.error?.type === "string") return frame.error.type;
  if (typeof frame?.error?.code === "string") return frame.error.code;
  if (typeof frame?.code === "string") return frame.code;
  return "AGENT_ERROR";
}

function pickErrorMessage(frame) {
  if (typeof frame?.error?.message === "string") return frame.error.message;
  if (typeof frame?.message === "string" && frame.type === "error") return frame.message;
  return "agent reported an error";
}

/**
 * Parse a single line into a frame (or null on bad JSON / blank line).
 * Stream-json is one-object-per-line, but we tolerate blank lines, leading
 * whitespace, and trailing CR.
 *
 * @param {string} line
 * @returns {object | null}
 */
function parseLine(line) {
  if (typeof line !== "string") return null;
  const trimmed = line.replace(/\r$/, "").trim();
  if (!trimmed) return null;
  if (!trimmed.startsWith("{") && !trimmed.startsWith("[")) return null;
  try {
    return JSON.parse(trimmed);
  } catch {
    return null;
  }
}

/**
 * A small helper: given a Readable stream of stdout, async-iterate each
 * non-empty line decoded as utf8.
 *
 * @param {NodeJS.ReadableStream} stdout
 * @returns {AsyncIterable<string>}
 */
async function* lineStream(stdout) {
  let buffer = "";
  stdout.setEncoding?.("utf8");
  for await (const chunk of stdout) {
    buffer += typeof chunk === "string" ? chunk : chunk.toString("utf8");
    let idx;
    while ((idx = buffer.indexOf("\n")) >= 0) {
      const line = buffer.slice(0, idx);
      buffer = buffer.slice(idx + 1);
      if (line) yield line;
    }
  }
  if (buffer) yield buffer;
}

module.exports = {
  frameToEvents,
  parseLine,
  lineStream,
  // exported for tests
  _internal: { collectFromContentBlock, pickText, pickToolName, pickSessionId, pickTokens },
};
