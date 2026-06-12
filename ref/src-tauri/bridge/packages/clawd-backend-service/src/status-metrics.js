"use strict";

const ACTIVE_TURN_STATES = new Set(["working", "thinking", "tool_running", "speaking", "waiting_user"]);
const TOOL_START_EVENTS = new Set([
  "PreToolUse",
  "BeforeTool",
  "response_item:function_call",
  "response_item:custom_tool_call",
  "response_item:web_search_call",
  "session.tool",
]);
const TOOL_ERROR_EVENTS = new Set([
  "PostToolUseFailure",
  "AfterToolError",
  "tool_error",
]);

function readFiniteNumber(...values) {
  for (const value of values) {
    if (typeof value === "number" && Number.isFinite(value)) return value;
  }
  return undefined;
}

function roundTo(value, digits) {
  const scale = 10 ** digits;
  return Math.round(value * scale) / scale;
}

function calculateContextUsagePct(tokenUsage) {
  if (!tokenUsage || typeof tokenUsage !== "object") return undefined;
  const totalTokens = readFiniteNumber(tokenUsage.totalTokens);
  const contextWindow = readFiniteNumber(tokenUsage.modelContextWindow, tokenUsage.contextTokens);
  if (!Number.isFinite(totalTokens) || !Number.isFinite(contextWindow) || contextWindow <= 0) return undefined;
  return roundTo((totalTokens / contextWindow) * 100, 2);
}

function normalizeLower(value) {
  return typeof value === "string" ? value.trim().toLowerCase() : "";
}

function buildToolSignature(payload) {
  const event = typeof payload.event === "string" ? payload.event : "";
  const reason = typeof payload.reason === "string" ? payload.reason : "";
  const detail = payload.detail && typeof payload.detail === "object" ? payload.detail : null;
  const toolName = detail && typeof detail.toolName === "string"
    ? detail.toolName
    : detail && typeof detail.tool === "string"
      ? detail.tool
      : "";
  return `${event}|${reason}|${toolName}`;
}

function isToolStartSignal(payload, state, previousState) {
  if (state === "tool_running" && previousState !== "tool_running") return true;
  const event = normalizeLower(payload.event);
  if (TOOL_START_EVENTS.has(payload.event)) return true;
  if (event.includes("function_call")) return true;
  if (event.includes("tooluse") && !event.includes("failure")) return true;
  const reason = normalizeLower(payload.reason);
  if (reason === "agent.tool" || reason === "session.tool") return true;
  if (reason.startsWith("agent.tool.") || reason.startsWith("session.tool.")) {
    if (reason.endsWith(".end") || reason.endsWith(".error")) return false;
    return true;
  }
  return false;
}

function isToolErrorSignal(payload, state) {
  if (state !== "error") return false;
  if (TOOL_ERROR_EVENTS.has(payload.event)) return true;
  const event = normalizeLower(payload.event);
  const reason = normalizeLower(payload.reason);
  if (event.includes("tool") || event.includes("function_call")) return true;
  if (reason.includes(".tool") || reason.includes("tool.")) return true;
  return false;
}

class SessionMetricsTracker {
  constructor(config = {}) {
    const ttlMs = Number.parseInt(config.sessionTtlMs, 10);
    this.sessionTtlMs = Number.isFinite(ttlMs) && ttlMs > 0 ? ttlMs : 30 * 60 * 1000;
    this.sessions = new Map();
  }

  apply(payload, nowMs = Date.now()) {
    if (!payload || typeof payload !== "object") return payload;

    const source = typeof payload.source === "string" && payload.source ? payload.source : "unknown";
    const scope = this._resolveScope(payload);
    const key = `${source}|${scope}`;
    const state = typeof payload.state === "string" ? payload.state : "idle";
    const sourceFallbackKey = `${source}|source`;
    const record = this._getOrCreateRecord(
      key,
      nowMs,
      scope === "source" ? null : sourceFallbackKey
    );
    record.lastSeenAt = nowMs;

    if (!record.turn && ACTIVE_TURN_STATES.has(state)) {
      this._startTurn(record, nowMs);
    }

    const previousState = record.lastState;
    const turn = record.turn;
    if (turn) {
      if (state === "speaking" && turn.firstTokenMs === undefined) {
        turn.firstTokenMs = Math.max(0, nowMs - turn.startedAt);
      }

      if (isToolStartSignal(payload, state, previousState)) {
        const signature = buildToolSignature(payload);
        if (signature && signature !== turn.lastToolSignature) {
          turn.toolCalls += 1;
          turn.lastToolSignature = signature;
        }
      }

      if (isToolErrorSignal(payload, state)) {
        const signature = buildToolSignature(payload) || `error|${payload.event || ""}|${payload.reason || ""}`;
        if (signature !== turn.lastToolErrorSignature) {
          turn.toolErrors += 1;
          turn.lastToolErrorSignature = signature;
        }
      }

      this._trackWaitingUser(turn, state, nowMs);
    }

    const contextUsagePct = calculateContextUsagePct(payload.tokenUsage);
    let metrics = this._buildMetrics(record, contextUsagePct, nowMs);

    if (record.turn && (state === "done" || state === "error")) {
      const finalTurn = record.turn;
      if (finalTurn.waitingUserSinceMs !== undefined) {
        finalTurn.waitingUserMs += Math.max(0, nowMs - finalTurn.waitingUserSinceMs);
        finalTurn.waitingUserSinceMs = undefined;
      }
      metrics = this._buildMetrics(record, contextUsagePct, nowMs);
      record.turn = null;
    }

    record.lastState = state;
    this._cleanup(nowMs);

    const hasMetrics = metrics && this._hasAnyMetrics(metrics);
    if (!hasMetrics) {
      if (!Object.prototype.hasOwnProperty.call(payload, "metrics")) return payload;
      const next = { ...payload };
      delete next.metrics;
      return next;
    }
    return { ...payload, metrics };
  }

  _resolveScope(payload) {
    if (typeof payload.sessionId === "string" && payload.sessionId) return `session:${payload.sessionId}`;
    if (typeof payload.runId === "string" && payload.runId) return `run:${payload.runId}`;
    if (typeof payload.sessionKey === "string" && payload.sessionKey) return `sessionKey:${payload.sessionKey}`;
    return "source";
  }

  _getOrCreateRecord(key, nowMs, sourceFallbackKey = null) {
    let record = this.sessions.get(key);
    if (record) return record;

    if (sourceFallbackKey && sourceFallbackKey !== key) {
      const fallback = this.sessions.get(sourceFallbackKey);
      // Compatibility path:
      // some PermissionRequest payloads may miss session_id, causing waiting_user to be tracked
      // on source scope. When a later event includes session key, migrate the in-flight turn.
      if (fallback && fallback.turn && !this.sessions.has(key)) {
        this.sessions.set(key, fallback);
        this.sessions.delete(sourceFallbackKey);
        return fallback;
      }
    }

    record = {
      lastSeenAt: nowMs,
      lastState: undefined,
      turn: null,
    };
    this.sessions.set(key, record);
    return record;
  }

  _startTurn(record, nowMs) {
    record.turn = {
      startedAt: nowMs,
      firstTokenMs: undefined,
      toolCalls: 0,
      toolErrors: 0,
      waitingUserMs: 0,
      waitingUserSinceMs: undefined,
      lastToolSignature: "",
      lastToolErrorSignature: "",
    };
  }

  _trackWaitingUser(turn, state, nowMs) {
    if (state === "waiting_user") {
      if (turn.waitingUserSinceMs === undefined) turn.waitingUserSinceMs = nowMs;
      return;
    }
    if (turn.waitingUserSinceMs !== undefined) {
      turn.waitingUserMs += Math.max(0, nowMs - turn.waitingUserSinceMs);
      turn.waitingUserSinceMs = undefined;
    }
  }

  _buildMetrics(record, contextUsagePct, nowMs) {
    const turn = record.turn;
    const latency = {};
    let toolCalls;
    let toolErrors;
    let waitingUserMs;

    if (turn) {
      latency.turnMs = Math.max(0, nowMs - turn.startedAt);
      if (Number.isFinite(turn.firstTokenMs)) latency.firstTokenMs = turn.firstTokenMs;
      toolCalls = turn.toolCalls;
      toolErrors = turn.toolErrors;
      waitingUserMs = turn.waitingUserMs;
      if (turn.waitingUserSinceMs !== undefined) {
        waitingUserMs += Math.max(0, nowMs - turn.waitingUserSinceMs);
      }
      waitingUserMs = Math.max(0, waitingUserMs);
    }

    const metrics = {};
    if (Object.keys(latency).length > 0) metrics.latency = latency;
    if (Number.isFinite(toolCalls)) metrics.toolCalls = toolCalls;
    if (Number.isFinite(toolErrors)) metrics.toolErrors = toolErrors;
    if (Number.isFinite(waitingUserMs)) metrics.waitingUserMs = waitingUserMs;
    if (Number.isFinite(contextUsagePct)) metrics.contextUsagePct = contextUsagePct;
    return metrics;
  }

  _hasAnyMetrics(metrics) {
    if (!metrics || typeof metrics !== "object") return false;
    if (Number.isFinite(metrics.contextUsagePct)) return true;
    if (Number.isFinite(metrics.toolCalls)) return true;
    if (Number.isFinite(metrics.toolErrors)) return true;
    if (Number.isFinite(metrics.waitingUserMs)) return true;
    const latency = metrics.latency;
    if (latency && typeof latency === "object") {
      if (Number.isFinite(latency.firstTokenMs)) return true;
      if (Number.isFinite(latency.turnMs)) return true;
    }
    return false;
  }

  _cleanup(nowMs) {
    for (const [key, record] of this.sessions.entries()) {
      if (nowMs - record.lastSeenAt > this.sessionTtlMs) {
        this.sessions.delete(key);
      }
    }
  }
}

module.exports = {
  SessionMetricsTracker,
  calculateContextUsagePct,
};
