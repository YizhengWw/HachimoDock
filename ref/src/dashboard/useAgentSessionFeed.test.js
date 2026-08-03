/**
 * [Input] Pure Agent Session feed reducer actions.
 * [Output] Behavioral coverage for Agent isolation, ordered terminal transitions, expiry, and deduplication.
 * [Pos] Test node for dashboard Session orchestration.
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  AGENT_SESSION_FEED_INITIAL_STATE,
  agentSessionFeedReducer,
} from "./useAgentSessionFeed.js";
import { deviceSessionTerminalUntilMs } from "../lib/session-display.js";

function reduce(state, action) {
  return agentSessionFeedReducer(state, {
    agentId: "codex",
    displayEnabled: true,
    dismissedSessions: {},
    ...action,
  });
}

test("Session feed admits active work and deduplicates an unchanged authoritative snapshot", () => {
  const working = {
    id: "session-a",
    name: "Active task",
    state: "working",
    lastModified: 1000,
    statusUpdatedAt: 1000,
  };
  const loaded = reduce(AGENT_SESSION_FEED_INITIAL_STATE, {
    type: "replace_snapshot",
    value: [working],
    nowMs: 1000,
  });

  assert.equal(loaded.agentId, "codex");
  assert.equal(loaded.loaded, true);
  assert.deepEqual(loaded.sessions.map((session) => session.id), ["session-a"]);
  assert.strictEqual(reduce(loaded, {
    type: "replace_snapshot",
    value: [working],
    nowMs: 1000,
  }), loaded);
});

test("Session feed preserves an explicit done transition for 60 seconds and then expires it", () => {
  const working = reduce(AGENT_SESSION_FEED_INITIAL_STATE, {
    type: "apply_event",
    value: {
      id: "session-a",
      name: "Active task",
      state: "working",
      lastModified: 1000,
      statusUpdatedAt: 1000,
    },
    nowMs: 1000,
  });
  const done = reduce(working, {
    type: "apply_event",
    value: {
      id: "session-a",
      state: "done",
      lastModified: 2000,
      statusUpdatedAt: 2000,
    },
    nowMs: 2000,
  });

  assert.equal(done.sessions.length, 1);
  assert.equal(done.sessions[0].state, "done");
  assert.equal(deviceSessionTerminalUntilMs(done.sessions[0]), 62_000);

  const expired = reduce(done, { type: "tick_terminal", nowMs: 62_001 });
  assert.deepEqual(expired.sessions, []);
  assert.equal(expired.routingSessions[0].id, "session-a");
});

test("Session feed treats Claude idle after visible work as a 60-second completion", () => {
  const working = reduce(AGENT_SESSION_FEED_INITIAL_STATE, {
    type: "replace_snapshot",
    agentId: "claude-code",
    value: [{
      id: "claude-task",
      name: "Claude task",
      state: "working",
      statusUpdatedAt: 10_000,
    }],
    nowMs: 10_000,
  });
  const idle = reduce(working, {
    type: "replace_snapshot",
    agentId: "claude-code",
    value: [{
      id: "claude-task",
      name: "Claude task",
      state: "idle",
      statusUpdatedAt: 11_000,
    }],
    nowMs: 11_000,
  });

  assert.equal(idle.sessions.length, 1);
  assert.equal(idle.sessions[0].state, "done");
  assert.equal(deviceSessionTerminalUntilMs(idle.sessions[0]), 71_000);
});

test("Session feed gates stale Agent state and replaces ownership on the next snapshot", () => {
  const codex = reduce(AGENT_SESSION_FEED_INITIAL_STATE, {
    type: "replace_snapshot",
    value: [{ id: "codex-a", name: "Codex task", state: "working", statusUpdatedAt: 1000 }],
    nowMs: 1000,
  });
  assert.strictEqual(
    agentSessionFeedReducer(codex, {
      type: "tick_terminal",
      agentId: "claude-code",
      nowMs: 2000,
      displayEnabled: true,
      dismissedSessions: {},
    }),
    codex,
  );

  const claude = agentSessionFeedReducer(codex, {
    type: "replace_snapshot",
    agentId: "claude-code",
    value: [{ id: "claude-a", name: "Claude task", state: "working", statusUpdatedAt: 3000 }],
    nowMs: 3000,
    displayEnabled: true,
    dismissedSessions: {},
  });
  assert.equal(claude.agentId, "claude-code");
  assert.deepEqual(claude.sessions.map((session) => session.id), ["claude-a"]);
  assert.strictEqual(
    agentSessionFeedReducer(claude, { type: "reset" }),
    AGENT_SESSION_FEED_INITIAL_STATE,
  );
});

test("Session feed keeps working lifecycle when a Codex metadata snapshot omits state", () => {
  const event = reduce(AGENT_SESSION_FEED_INITIAL_STATE, {
    type: "apply_event",
    value: {
      id: "codex-new",
      state: "working",
      displayTitle: "Codex 会话",
      statusUpdatedAt: 1000,
    },
    nowMs: 1000,
  });
  assert.deepEqual(event.sessions, []);

  const snapshot = reduce(event, {
    type: "replace_snapshot",
    value: [{
      id: "codex-new",
      summary: "Refactor the session router",
      lastModified: 2000,
    }],
    nowMs: 2000,
  });
  assert.equal(snapshot.routingSessions[0].state, "working");
  assert.deepEqual(snapshot.sessions.map((session) => session.id), ["codex-new"]);
});
