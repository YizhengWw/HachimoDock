"use strict";

/*
 * [Input] Normalized Agent events with session, display, path, and cumulative/per-turn token metadata.
 * [Output] Per-session live metrics, retained token usage, and a bounded ordered lifecycle-event stream for device consumers.
 * [Pos] In-memory session enrichment store for the managed bridge.
 */

const crypto = require("crypto");

const ACTIVE_TURN_STATES = new Set(["working", "thinking", "tool_running", "speaking", "waiting_user"]);
const TERMINAL_TURN_STATES = new Set(["done", "error"]);
const DEFAULT_SESSION_EVENT_LIMIT = 512;
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
  const usedTokens = readFiniteNumber(
    tokenUsage.currentContextTokens,
    tokenUsage.contextUsedTokens,
    tokenUsage.lastInputTokens,
    tokenUsage.lastTotalTokens,
    tokenUsage.inputTokens,
    tokenUsage.totalTokens
  );
  const contextWindow = readFiniteNumber(tokenUsage.modelContextWindow, tokenUsage.contextTokens);
  if (!Number.isFinite(usedTokens) || !Number.isFinite(contextWindow) || contextWindow <= 0) return undefined;
  return Math.min(100, roundTo((usedTokens / contextWindow) * 100, 2));
}

function readTokenUsage(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  return Object.values(value).some(Number.isFinite) ? { ...value } : null;
}

function normalizeLower(value) {
  return typeof value === "string" ? value.trim().toLowerCase() : "";
}

function isNewTurnStartSignal(payload) {
  const event = normalizeLower(payload?.event);
  const reason = normalizeLower(payload?.reason);
  return [
    "userpromptsubmit",
    "user_message",
    "task_started",
    "session.start",
    "session.pre",
    "agent.lifecycle.start",
    "sessions.changed.start",
    "sessions.changed.send",
    "sessions.changed.steer",
  ].some((marker) => event.includes(marker) || reason.includes(marker));
}

function shouldPreserveTerminalState(payload, incomingState, previousState) {
  if (!TERMINAL_TURN_STATES.has(previousState)) return false;
  if (TERMINAL_TURN_STATES.has(incomingState)) return false;
  return !isNewTurnStartSignal(payload);
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
    const eventLimit = Number.parseInt(config.sessionEventLimit, 10);
    this.sessionTtlMs = Number.isFinite(ttlMs) && ttlMs > 0 ? ttlMs : 30 * 60 * 1000;
    this.sessionEventLimit = Number.isFinite(eventLimit) && eventLimit > 0
      ? eventLimit
      : DEFAULT_SESSION_EVENT_LIMIT;
    this.sessions = new Map();
    this.sessionEventSequence = 0;
    this.sessionEventStreamId = crypto.randomUUID();
    this.sessionEventLog = [];
    this.sessionEventSignatures = new Map();
  }

  apply(payload, nowMs = Date.now()) {
    if (!payload || typeof payload !== "object") return payload;

    const source = normalizeLower(payload.source) || "unknown";
    const scope = this._resolveScope(payload);
    const key = `${source}|${scope}`;
    const incomingState = typeof payload.state === "string" ? payload.state : "idle";
    const sourceFallbackKey = `${source}|source`;
    const record = this._getOrCreateRecord(
      key,
      nowMs,
      scope === "source" ? null : sourceFallbackKey
    );
    const previousState = record.lastState;
    const preserveTerminalState = shouldPreserveTerminalState(
      payload,
      incomingState,
      previousState
    );
    const state = preserveTerminalState ? previousState : incomingState;
    const display = payload.display && typeof payload.display === "object" ? payload.display : null;
    const displayTitle = typeof display?.title === "string" ? display.title.trim() : "";
    const displayContent = typeof display?.content === "string" ? display.content.trim() : "";
    if (!preserveTerminalState) {
      record.lastSeenAt = nowMs;
      if (displayTitle) record.displayTitle = displayTitle;
      if (displayContent) record.displayContent = displayContent;
    }
    const sessionTitle = typeof payload.sessionTitle === "string" ? payload.sessionTitle.trim() : "";
    const cwd = typeof payload.cwd === "string" ? payload.cwd.trim() : "";
    const transcriptPath = typeof payload.transcriptPath === "string" ? payload.transcriptPath.trim() : "";
    if (sessionTitle && (!record.sessionTitle || payload.sessionTitleExplicit === true)) {
      record.sessionTitle = sessionTitle;
    }
    if (cwd) record.cwd = cwd;
    if (transcriptPath) record.transcriptPath = transcriptPath;
    const incomingTokenUsage = readTokenUsage(payload.tokenUsage);
    if (incomingTokenUsage) record.tokenUsage = incomingTokenUsage;
    const tokenUsage = incomingTokenUsage || record.tokenUsage || null;

    if (!record.turn && ACTIVE_TURN_STATES.has(state)) {
      this._startTurn(record, nowMs);
    }

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

    const contextUsagePct = calculateContextUsagePct(tokenUsage);
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

    let effectivePayload = preserveTerminalState
      ? {
        ...payload,
        state,
        ...(record.displayTitle || record.displayContent
          ? {
            display: {
              title: record.displayTitle || "",
              content: record.displayContent || "",
              status: state,
              event: payload.event,
              updatedAtMs: record.lastSeenAt,
            },
          }
          : {}),
      }
      : payload;
    if (tokenUsage && tokenUsage !== payload.tokenUsage) {
      effectivePayload = { ...effectivePayload, tokenUsage };
    }
    const hasMetrics = metrics && this._hasAnyMetrics(metrics);
    let result = effectivePayload;
    if (!hasMetrics && Object.prototype.hasOwnProperty.call(effectivePayload, "metrics")) {
      result = { ...effectivePayload };
      delete result.metrics;
    } else if (hasMetrics) {
      result = { ...effectivePayload, metrics };
    }
    this._recordSessionEvent(source, scope, record, result, nowMs);
    return result;
  }

  sessionStatuses(source, nowMs = Date.now()) {
    this._cleanup(nowMs);
    const normalizedSource = typeof source === "string" ? source.trim().toLowerCase() : "";
    const prefix = `${normalizedSource}|session:`;
    const statuses = [];
    for (const [key, record] of this.sessions.entries()) {
      if (!key.startsWith(prefix)) continue;
      statuses.push({
        id: key.slice(prefix.length),
        state: record.lastState || "idle",
        updatedAt: record.lastSeenAt || 0,
        ...(record.displayTitle ? { displayTitle: record.displayTitle } : {}),
        ...(record.displayContent ? { displayContent: record.displayContent } : {}),
        ...(record.sessionTitle ? { title: record.sessionTitle } : {}),
        ...(record.cwd ? { cwd: record.cwd } : {}),
        ...(record.transcriptPath ? { transcriptPath: record.transcriptPath } : {}),
      });
    }
    return statuses;
  }

  sessionEvents(source, options = {}) {
    const normalizedSource = typeof source === "string" ? source.trim().toLowerCase() : "";
    const streamId = typeof options.streamId === "string" ? options.streamId.trim() : "";
    const hasCursor = Number.isFinite(Number(options.cursor));
    const cursor = hasCursor ? Math.max(0, Math.floor(Number(options.cursor))) : null;
    const requestedLimit = Number.parseInt(options.limit, 10);
    const limit = Number.isFinite(requestedLimit) && requestedLimit > 0
      ? Math.min(requestedLimit, this.sessionEventLimit)
      : 100;
    const streamChanged = Boolean(streamId && streamId !== this.sessionEventStreamId);
    const latestCursor = this.sessionEventSequence;
    const oldestCursor = this.sessionEventLog.length > 0
      ? this.sessionEventLog[0].cursor
      : latestCursor + 1;
    const cursorExpired = cursor !== null
      && (cursor > latestCursor || cursor < oldestCursor - 1);

    if (cursor === null || streamChanged || cursorExpired) {
      const events = this.sessionStatuses(normalizedSource)
        .filter((status) => ACTIVE_TURN_STATES.has(normalizeLower(status.state)))
        .map((status) => ({
          cursor: latestCursor,
          source: normalizedSource,
          state: status.state,
          event: "session.bootstrap",
          updatedAt: status.updatedAt,
          session: this._statusToEventSession(status),
          bootstrap: true,
        }));
      return {
        streamId: this.sessionEventStreamId,
        cursor: latestCursor,
        reset: streamChanged || cursorExpired,
        events,
      };
    }

    const matching = this.sessionEventLog.filter(
      (event) => event.cursor > cursor && event.source === normalizedSource
    );
    const events = matching.slice(0, limit);
    const nextCursor = matching.length > limit && events.length > 0
      ? events[events.length - 1].cursor
      : latestCursor;
    return {
      streamId: this.sessionEventStreamId,
      cursor: nextCursor,
      reset: false,
      events,
    };
  }

  _statusToEventSession(status) {
    return {
      id: status.id,
      state: status.state || "idle",
      statusUpdatedAt: status.updatedAt || 0,
      ...(status.title ? { name: status.title, summary: status.title } : {}),
      ...(status.displayTitle ? { displayTitle: status.displayTitle } : {}),
      ...(status.displayContent ? { displayContent: status.displayContent } : {}),
      ...(status.cwd ? { cwd: status.cwd } : {}),
      ...(status.transcriptPath ? { transcriptPath: status.transcriptPath } : {}),
    };
  }

  _recordSessionEvent(source, scope, record, payload, nowMs) {
    if (!scope.startsWith("session:")) return;
    const id = scope.slice("session:".length).trim();
    if (!id) return;
    const state = normalizeLower(payload?.state) || "idle";
    const status = {
      id,
      state,
      updatedAt: record.lastSeenAt || nowMs,
      ...(record.displayTitle ? { displayTitle: record.displayTitle } : {}),
      ...(record.displayContent ? { displayContent: record.displayContent } : {}),
      ...(record.sessionTitle ? { title: record.sessionTitle } : {}),
      ...(record.cwd ? { cwd: record.cwd } : {}),
      ...(record.transcriptPath ? { transcriptPath: record.transcriptPath } : {}),
    };
    const signatureKey = `${source}|${id}`;
    const signature = JSON.stringify([
      state,
      status.displayTitle || "",
      status.displayContent || "",
      status.title || "",
      status.cwd || "",
      status.transcriptPath || "",
    ]);
    if (this.sessionEventSignatures.get(signatureKey) === signature) return;
    this.sessionEventSignatures.set(signatureKey, signature);

    const event = {
      cursor: ++this.sessionEventSequence,
      source: normalizeLower(source),
      state,
      event: typeof payload?.event === "string" ? payload.event : "",
      reason: typeof payload?.reason === "string" ? payload.reason : "",
      updatedAt: status.updatedAt,
      session: this._statusToEventSession(status),
    };
    this.sessionEventLog.push(event);
    if (this.sessionEventLog.length > this.sessionEventLimit) {
      this.sessionEventLog.splice(0, this.sessionEventLog.length - this.sessionEventLimit);
    }
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
        const separator = key.indexOf("|session:");
        if (separator >= 0) {
          const source = key.slice(0, separator);
          const id = key.slice(separator + "|session:".length);
          this.sessionEventSignatures.delete(`${source}|${id}`);
        }
      }
    }
  }
}

module.exports = {
  SessionMetricsTracker,
  calculateContextUsagePct,
};
