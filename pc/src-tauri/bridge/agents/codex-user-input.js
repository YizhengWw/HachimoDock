"use strict";

function object(value) {
  if (value && typeof value === "object") return value;
  if (typeof value !== "string" || value.length > 128 * 1024) return null;
  try { return JSON.parse(value); } catch { return null; }
}

// Observe actual Codex tool receipts, never inferred question punctuation.
class CodexUserInputTracker {
  constructor() { this.calls = new Map(); this.closed = new Set(); }
  get waiting() {
    return [...this.calls.values()].some(call => call.active && call.remaining.size > 0);
  }
  clear() { this.calls.clear(); }
  close(id) {
    this.calls.delete(id);
    this.closed.add(id);
    while (this.closed.size > 64) this.closed.delete(this.closed.values().next().value);
  }
  observe(key, payload) {
    const before = this.waiting;
    if (key === "event_msg:task_complete" || key === "event_msg:turn_aborted") {
      this.clear();
    } else if (key === "response_item:function_call") {
      const name = String(payload?.name || "").split(".").at(-1);
      const id = payload?.call_id;
      const args = object(payload?.arguments);
      if (["request_user_input", "request_user_input_async"].includes(name)
          && typeof id === "string" && id.length <= 256 && !this.closed.has(id)
          && !this.calls.has(id) && Array.isArray(args?.questions)
          && args.questions.length > 0 && args.questions.length <= 16) {
        if (this.calls.size >= 8) this.close(this.calls.keys().next().value);
        this.calls.set(id, { async: name.endsWith("_async"),
          active: !name.endsWith("_async"),
          remaining: new Set(args.questions.map((_, index) => index)) });
      }
    } else if (key === "response_item:function_call_output") {
      const call = this.calls.get(payload?.call_id);
      if (call) {
        if (call.async && object(payload?.output)?.accepted === true) call.active = true;
        else this.close(payload.call_id);
      }
    } else if (key === "event_msg:user_message") {
      const text = typeof payload?.message === "string" ? payload.message : "";
      const open = "<send_user_message_question_reply>";
      const close = "</send_user_message_question_reply>";
      const start = text.indexOf(open), end = text.indexOf(close);
      const replies = start >= 0 && end > start
        ? object(text.slice(start + open.length, end)) : null;
      if (Array.isArray(replies)) for (const reply of replies) {
        const item = object(reply?.questionItemId);
        if (!Array.isArray(item) || item[0] !== "request_user_input_async"
            || !Number.isInteger(item[2])) continue;
        const call = this.calls.get(item[1]);
        if (!call?.async) continue;
        call.remaining.delete(item[2]);
        if (call.remaining.size === 0) this.close(item[1]);
      }
    }
    return before !== this.waiting;
  }
}

module.exports = { CodexUserInputTracker };
