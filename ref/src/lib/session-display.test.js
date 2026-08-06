/**
 * [Input] Global session visibility helpers plus ordered Agent lifecycle events and periodic snapshots.
 * [Output] Regression coverage for persistence/migration plus adjacent-snapshot
 *          active admission, transient visual-identity dedupe, and 60-second done/error retention.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  DEVICE_SESSION_SNAPSHOT_MISSING_GRACE_MS,
  DEVICE_SESSION_TERMINAL_HOLD_MS,
  DEFAULT_SESSION_DISPLAY_ENABLED,
  SESSION_DISPLAY_ENABLED_STORAGE_KEY,
  deviceSessionTerminalRemainingMs,
  deviceSessionTransitionRevision,
  filterActiveDeviceSessions,
  isActiveDeviceSession,
  loadSessionDisplayEnabled,
  normalizeSessionDisplayEnabled,
  reconcileDeviceSessionQueue,
  saveSessionDisplayEnabled,
} from "./session-display.js";

function createStorage(initialValues = {}) {
  const values = new Map();
  for (const [key, value] of Object.entries(initialValues)) {
    if (value != null) values.set(key, String(value));
  }
  return {
    getItem(key) {
      return values.has(key) ? values.get(key) : null;
    },
    setItem(key, value) {
      values.set(key, String(value));
    },
  };
}

test("normalizes the global device-session visibility setting", () => {
  assert.equal(normalizeSessionDisplayEnabled(true), true);
  assert.equal(normalizeSessionDisplayEnabled("true"), true);
  assert.equal(normalizeSessionDisplayEnabled(false), false);
  assert.equal(normalizeSessionDisplayEnabled("false"), false);
  assert.equal(
    normalizeSessionDisplayEnabled("invalid"),
    DEFAULT_SESSION_DISPLAY_ENABLED,
  );
});

test("loads and saves whether active device sessions are shown", () => {
  const storage = createStorage({
    [SESSION_DISPLAY_ENABLED_STORAGE_KEY]: "false",
  });
  assert.equal(loadSessionDisplayEnabled(storage), false);
  assert.equal(saveSessionDisplayEnabled(true, storage), true);
  assert.equal(loadSessionDisplayEnabled(storage), true);

  const emptyStorage = createStorage();
  assert.equal(
    loadSessionDisplayEnabled(emptyStorage),
    DEFAULT_SESSION_DISPLAY_ENABLED,
  );
});

test("migrates the previous Codex-only values into the global setting", () => {
  const storage = createStorage({
    "pet-manager.codex-session-display-enabled": "false",
  });
  assert.equal(loadSessionDisplayEnabled(storage), false);
  assert.equal(saveSessionDisplayEnabled(true, storage), true);
  assert.equal(storage.getItem(SESSION_DISPLAY_ENABLED_STORAGE_KEY), "true");
});

test("only active lifecycle states contribute device session cards", () => {
  const activeStates = ["working", "thinking", "tool_running", "speaking", "waiting_user"];
  for (const state of activeStates) {
    assert.equal(isActiveDeviceSession({ state }), true, state);
  }
  for (const state of ["idle", "done", "error", "", null]) {
    assert.equal(isActiveDeviceSession({ state }), false, String(state));
  }
  assert.deepEqual(
    filterActiveDeviceSessions([
      { id: "working", state: "working" },
      { id: "done", state: "done" },
      { id: "waiting", state: "WAITING_USER" },
    ]).map((session) => session.id),
    ["working", "waiting"],
  );
});

test("cold snapshots admit active sessions but never import terminal or idle history", () => {
  const nowMs = 2_000_000;
  const sessions = [
    { id: "working", state: "working", statusUpdatedAt: nowMs - 120_000 },
    { id: "waiting", state: "waiting_user", statusUpdatedAt: nowMs - 120_000 },
    { id: "done-recent", state: "done", statusUpdatedAt: nowMs },
    { id: "error-recent", state: "error", statusUpdatedAt: nowMs },
    { id: "idle", state: "idle", statusUpdatedAt: nowMs },
  ];

  assert.deepEqual(
    reconcileDeviceSessionQueue([], sessions, nowMs).map((session) => session.id),
    ["working", "waiting"],
  );
});

test("retains active to done transitions for exactly 60 seconds", () => {
  const startedAt = 3_000_000;
  const active = reconcileDeviceSessionQueue(
    [],
    [{ id: "task", state: "working", title: "Task" }],
    startedAt,
  );
  const terminal = reconcileDeviceSessionQueue(
    active,
    [{ id: "task", state: "done", title: "Task" }],
    startedAt + 1,
  );

  assert.equal(DEVICE_SESSION_TERMINAL_HOLD_MS, 60_000);
  assert.deepEqual(terminal.map((session) => session.state), ["done"]);
  assert.equal(
    reconcileDeviceSessionQueue(terminal, [], startedAt + 60_000).length,
    1,
  );
  assert.equal(
    reconcileDeviceSessionQueue(terminal, [], startedAt + 60_001).length,
    0,
  );
});

test("retains active to error transitions without extending the original deadline", () => {
  const startedAt = 4_000_000;
  const active = reconcileDeviceSessionQueue(
    [],
    [{ id: "task", state: "thinking", title: "Task" }],
    startedAt,
  );
  const terminal = reconcileDeviceSessionQueue(
    active,
    [{ id: "task", state: "failed", title: "Task failed" }],
    startedAt + 10,
  );
  const refreshed = reconcileDeviceSessionQueue(
    terminal,
    [{ id: "task", state: "error", title: "Task failed" }],
    startedAt + 30_000,
  );

  assert.deepEqual(refreshed.map((session) => session.state), ["error"]);
  assert.equal(
    reconcileDeviceSessionQueue(refreshed, [], startedAt + 60_009).length,
    1,
  );
  assert.equal(
    reconcileDeviceSessionQueue(refreshed, [], startedAt + 60_010).length,
    0,
  );
});

test("assigns stable transition revisions and exposes only the remaining terminal TTL", () => {
  const startedAt = 4_500_000;
  const active = reconcileDeviceSessionQueue(
    [],
    [{ id: "task", state: "working", statusUpdatedAt: startedAt - 10 }],
    startedAt,
  );
  const repeatedActive = reconcileDeviceSessionQueue(
    active,
    [{ id: "task", state: "working", statusUpdatedAt: startedAt - 10 }],
    startedAt + 500,
  );
  const thinking = reconcileDeviceSessionQueue(
    repeatedActive,
    [{ id: "task", state: "thinking", statusUpdatedAt: startedAt + 1_000 }],
    startedAt + 1_000,
  );
  const terminal = reconcileDeviceSessionQueue(
    thinking,
    [{ id: "task", state: "completed", statusUpdatedAt: startedAt + 2_000 }],
    startedAt + 2_000,
  );
  const repeatedTerminal = reconcileDeviceSessionQueue(
    terminal,
    [{ id: "task", state: "done", statusUpdatedAt: startedAt + 2_000 }],
    startedAt + 32_000,
  );

  assert.equal(
    deviceSessionTransitionRevision(repeatedActive[0]),
    deviceSessionTransitionRevision(active[0]),
  );
  assert.ok(
    deviceSessionTransitionRevision(thinking[0])
      > deviceSessionTransitionRevision(repeatedActive[0]),
  );
  assert.ok(
    deviceSessionTransitionRevision(terminal[0])
      > deviceSessionTransitionRevision(thinking[0]),
  );
  assert.equal(
    deviceSessionTransitionRevision(repeatedTerminal[0]),
    deviceSessionTransitionRevision(terminal[0]),
  );
  assert.equal(
    deviceSessionTerminalRemainingMs(repeatedTerminal[0], startedAt + 32_000),
    30_000,
  );
  assert.equal(
    deviceSessionTerminalRemainingMs(repeatedTerminal[0], startedAt + 62_000),
    0,
  );
});

test("reactivating a retained terminal session creates a newer transition", () => {
  const startedAt = 4_800_000;
  const active = reconcileDeviceSessionQueue(
    [],
    [{ id: "task", state: "working" }],
    startedAt,
  );
  const terminal = reconcileDeviceSessionQueue(
    active,
    [{ id: "task", state: "error" }],
    startedAt + 1,
  );
  const resumed = reconcileDeviceSessionQueue(
    terminal,
    [{ id: "task", state: "working" }],
    startedAt + 2,
  );

  assert.ok(
    deviceSessionTransitionRevision(resumed[0])
      > deviceSessionTransitionRevision(terminal[0]),
  );
  assert.equal(deviceSessionTerminalRemainingMs(resumed[0], startedAt + 2), 0);
});

test("keeps snapshot-omitted active sessions and retains explicit idle as done", () => {
  const nowMs = 5_000_000;
  const active = reconcileDeviceSessionQueue(
    [],
    [
      { id: "missing", state: "working" },
      { id: "idle", state: "working" },
    ],
    nowMs,
  );
  const next = reconcileDeviceSessionQueue(
    active,
    [{ id: "idle", state: "idle" }],
    nowMs + 1,
  );

  assert.deepEqual(
    next.map((session) => [session.id, session.state]),
    [["missing", "working"], ["idle", "done"]],
  );
  assert.equal(
    deviceSessionTerminalRemainingMs(next[1], nowMs + 1),
    DEVICE_SESSION_TERMINAL_HOLD_MS,
  );
});

test("authoritative snapshots tolerate brief omission then retain an implicit completion", () => {
  const nowMs = 5_500_000;
  const active = reconcileDeviceSessionQueue(
    [],
    [{ id: "stale", state: "working" }],
    nowMs,
  );

  const firstMiss = reconcileDeviceSessionQueue(
    active,
    [],
    nowMs + 1,
    8,
    { authoritativeSnapshot: true },
  );
  assert.equal(firstMiss.length, 1);
  assert.equal(
    reconcileDeviceSessionQueue(
      firstMiss,
      [],
      nowMs + DEVICE_SESSION_SNAPSHOT_MISSING_GRACE_MS,
      8,
      { authoritativeSnapshot: true },
    ).length,
    1,
  );
  const implicitTerminal = reconcileDeviceSessionQueue(
    firstMiss,
    [],
    nowMs + DEVICE_SESSION_SNAPSHOT_MISSING_GRACE_MS + 1,
    8,
    { authoritativeSnapshot: true },
  );
  assert.equal(implicitTerminal.length, 1);
  assert.equal(implicitTerminal[0].state, "done");
  assert.equal(
    reconcileDeviceSessionQueue(
      implicitTerminal,
      [],
      nowMs + DEVICE_SESSION_TERMINAL_HOLD_MS,
      8,
      { authoritativeSnapshot: true },
    ).length,
    1,
  );
  assert.equal(
    reconcileDeviceSessionQueue(
      implicitTerminal,
      [],
      nowMs + DEVICE_SESSION_TERMINAL_HOLD_MS + 1,
      8,
      { authoritativeSnapshot: true },
    ).length,
    0,
  );
});

test("a cold Agent snapshot cannot revive terminal cards from another Agent or app run", () => {
  const previousAgentQueue = reconcileDeviceSessionQueue(
    [],
    [{ id: "shared-id", state: "working" }],
    6_000_000,
  );
  assert.equal(previousAgentQueue.length, 1);

  assert.deepEqual(
    reconcileDeviceSessionQueue(
      [],
      [
        { id: "shared-id", state: "done" },
        { id: "other-error", state: "error" },
      ],
      6_000_100,
    ),
    [],
  );
});

test("active sessions take every available slot before retained terminal cards", () => {
  const nowMs = 7_000_000;
  const previous = reconcileDeviceSessionQueue(
    [],
    [
      { id: "old-a", state: "working" },
      { id: "old-b", state: "working" },
    ],
    nowMs,
  );
  const incoming = [
    { id: "old-a", state: "done" },
    { id: "old-b", state: "error" },
    ...Array.from({ length: 8 }, (_, index) => ({
      id: `active-${index}`,
      state: "working",
    })),
  ];
  const next = reconcileDeviceSessionQueue(previous, incoming, nowMs + 1, 8);

  assert.equal(next.length, 8);
  assert.deepEqual(
    next.map((session) => session.id),
    incoming.slice(2).map((session) => session.id),
  );
});

test("collapses transient duplicate identities from event and snapshot feeds", () => {
  const nowMs = 7_500_000;
  const previous = reconcileDeviceSessionQueue(
    [],
    [
      {
        id: "snapshot-id",
        state: "working",
        name: "同一个任务",
        displayContent: "正在执行",
        cwd: "/tmp/project",
        statusUpdatedAt: nowMs,
      },
      {
        id: "event-id",
        state: "working",
        displayTitle: "同一个任务",
        displayContent: "正在执行",
        cwd: "/tmp/project",
        statusUpdatedAt: nowMs + 1,
      },
    ],
    nowMs + 1,
  );

  assert.equal(previous.length, 1);
  assert.equal(previous[0].id, "event-id");

  const terminal = reconcileDeviceSessionQueue(
    [
      {
        id: "snapshot-id",
        state: "working",
        name: "同一个任务",
        displayContent: "已完成",
        cwd: "/tmp/project",
        statusUpdatedAt: nowMs,
      },
      {
        id: "event-id",
        state: "working",
        displayTitle: "同一个任务",
        displayContent: "已完成",
        cwd: "/tmp/project",
        statusUpdatedAt: nowMs,
      },
    ],
    [
      {
        id: "snapshot-id",
        state: "working",
        name: "同一个任务",
        displayContent: "已完成",
        cwd: "/tmp/project",
        statusUpdatedAt: nowMs,
      },
      {
        id: "event-id",
        state: "done",
        displayTitle: "同一个任务",
        displayContent: "已完成",
        cwd: "/tmp/project",
        statusUpdatedAt: nowMs + 2,
      },
    ],
    nowMs + 2,
  );

  assert.equal(terminal.length, 1);
  assert.equal(terminal[0].id, "event-id");
  assert.equal(terminal[0].state, "done");
});

test("collapses one visible Session even when event and snapshot metadata paths differ", () => {
  const nowMs = 7_600_000;
  const next = reconcileDeviceSessionQueue(
    [],
    [
      {
        id: "snapshot-id",
        state: "working",
        name: "同一个任务",
        displayContent: "正在执行",
        cwd: "/tmp/snapshot-project",
        transcriptPath: "/tmp/snapshot.jsonl",
        statusUpdatedAt: nowMs,
      },
      {
        id: "event-id",
        state: "working",
        displayTitle: "同一个任务",
        displayContent: "正在执行",
        cwd: "/tmp/event-project",
        transcriptPath: "",
        statusUpdatedAt: nowMs + 1,
      },
    ],
    nowMs + 1,
  );

  assert.equal(next.length, 1);
  assert.equal(next[0].id, "event-id");
});

test("deduplicates title-only cards using the lifecycle text rendered by firmware", () => {
  const nowMs = 7_700_000;
  const next = reconcileDeviceSessionQueue(
    [],
    [
      { id: "snapshot-id", state: "working", name: "同一个任务", statusUpdatedAt: nowMs },
      { id: "event-id", state: "working", displayTitle: "同一个任务", statusUpdatedAt: nowMs + 1 },
    ],
    nowMs + 1,
  );

  assert.equal(next.length, 1);
  assert.equal(next[0].id, "event-id");
});

test("falls back safely when browser storage is unavailable", () => {
  const unavailableStorage = {
    getItem() {
      throw new Error("unavailable");
    },
    setItem() {
      throw new Error("unavailable");
    },
  };
  assert.equal(
    loadSessionDisplayEnabled(unavailableStorage),
    DEFAULT_SESSION_DISPLAY_ENABLED,
  );
  assert.equal(
    saveSessionDisplayEnabled(false, unavailableStorage),
    false,
  );
});
