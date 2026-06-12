"use strict";

const { BaseAdapter } = require("./base");

/**
 * MockAdapter — emits a canned token stream so the rest of the pipeline
 * (HTTP/SSE wiring in the bus, voice-service-node refactor, UI dropdowns,
 * E2E plumbing) can be developed and tested on machines that don't have
 * a real coding agent installed.
 *
 * Drives a hardcoded set of fake sessions in memory. Picked up by the
 * bus when AGENT_BUS_USE_MOCK=1 or when explicitly registered.
 */
class MockAdapter extends BaseAdapter {
  constructor({ agentId = "mock", log, replyText, tokensPerSecond = 8 } = {}) {
    super({ agentId, log });
    this._sessions = [
      { id: "mock-session-001", lastModified: Date.now() - 60_000, summary: "上次：refactor src/main.rs" },
      { id: "mock-session-002", lastModified: Date.now() - 86_400_000, summary: "昨天：fix lint" },
    ];
    this._replyText = replyText || "好的，已经按你的语音指令执行完毕。";
    this._tokensPerSecond = Math.max(1, Math.min(60, tokensPerSecond));
  }

  async isAvailable() {
    return { ready: true };
  }

  async listSessions({ limit = 10 } = {}) {
    return this._sessions.slice(0, limit);
  }

  async openNew(/* _opts */) {
    const id = `mock-session-${Date.now()}`;
    const ref = { id, lastModified: Date.now(), summary: "new (mock)" };
    this._sessions.unshift(ref);
    return ref;
  }

  async *inject({ sessionId, text, signal }) {
    if (sessionId && sessionId !== "auto") {
      const found = this._sessions.find((s) => s.id === sessionId);
      if (!found) {
        yield {
          kind: "error",
          code: "SESSION_NOT_FOUND",
          message: `mock has no session ${sessionId}`,
        };
        return;
      }
    }

    const reply = `${this._replyText} (回复："${text.trim()}")`;
    const tokens = Array.from(reply);
    const delayMs = Math.max(10, Math.floor(1000 / this._tokensPerSecond));
    let cancelled = false;
    const onAbort = () => { cancelled = true; };
    if (signal) {
      if (signal.aborted) cancelled = true;
      else signal.addEventListener("abort", onAbort, { once: true });
    }

    try {
      for (const ch of tokens) {
        if (cancelled) {
          yield { kind: "error", code: "CANCELLED", message: "mock inject aborted" };
          return;
        }
        yield { kind: "token", text: ch };
        await new Promise((resolve) => setTimeout(resolve, delayMs));
      }
      const sid = sessionId === "auto" || !sessionId
        ? this._sessions[0].id
        : sessionId;
      yield {
        kind: "done",
        sessionId: sid,
        tokens: tokens.length,
        stopReason: "end_turn",
      };
    } finally {
      if (signal) signal.removeEventListener("abort", onAbort);
    }
  }
}

module.exports = {
  MockAdapter,
};
