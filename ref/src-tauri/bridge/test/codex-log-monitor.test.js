"use strict";

/*
 * [Input] Synthetic Codex rollout directories and append events.
 * [Output] Regression coverage for low-cost discovery, arbitrary-age active recovery, and prompt live updates.
 * [Pos] Unit tests for the managed bridge's Codex rollout monitor.
 */

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { test } = require("node:test");

const CodexLogMonitor = require("../agents/codex-log-monitor");
const codexDefaults = require("../agents/codex");

function dayDir(root, date = new Date()) {
  return path.join(
    root,
    String(date.getFullYear()),
    String(date.getMonth() + 1).padStart(2, "0"),
    String(date.getDate()).padStart(2, "0"),
  );
}

function rolloutName(index) {
  const suffix = String(index).padStart(12, "0");
  return `rollout-test-00000000-0000-4000-8000-${suffix}.jsonl`;
}

function writeJsonl(filePath, records) {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  fs.writeFileSync(
    filePath,
    `${records.map((record) => JSON.stringify(record)).join("\n")}\n`,
  );
}

test("CodexLogMonitor keeps non-Windows fallback discovery at the polling cadence", () => {
  assert.equal(
    codexDefaults.DISCOVERY_INTERVAL_MS,
    process.platform === "win32"
      ? 30_000
      : codexDefaults.POLL_INTERVAL_MS,
  );
});

test("CodexLogMonitor fast polls only tracked files instead of restatting history", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "codex-log-monitor-cost-"));
  try {
    const root = path.join(home, "sessions");
    const dir = dayDir(root);
    fs.mkdirSync(dir, { recursive: true });
    const oldTime = new Date(Date.now() - 10 * 60 * 1000);

    for (let index = 0; index < 100; index += 1) {
      const filePath = path.join(dir, rolloutName(index));
      fs.writeFileSync(filePath, "");
      fs.utimesSync(filePath, oldTime, oldTime);
    }
    const activePath = path.join(dir, rolloutName(999));
    fs.writeFileSync(activePath, "");

    const monitor = new CodexLogMonitor({
      SESSION_DIR: root,
      SESSION_INDEX_PATH: "",
      LOOKBACK_DAYS: 1,
      POLL_INTERVAL_MS: 1500,
      DISCOVERY_INTERVAL_MS: 60000,
      NEW_FILE_MAX_AGE_MS: 120000,
      INITIAL_TAIL_BYTES: 0,
      WATCH_FILES: false,
    });
    monitor.baselineExistingSessions();

    assert.equal(monitor.knownFiles.size, 101);
    assert.equal(monitor.tracked.size, 1);

    const originalStatSync = fs.statSync;
    let sessionStatCalls = 0;
    fs.statSync = function countedStatSync(filePath, ...args) {
      if (String(filePath).startsWith(root)) sessionStatCalls += 1;
      return originalStatSync.call(this, filePath, ...args);
    };
    try {
      for (let index = 0; index < 5; index += 1) monitor.poll();
    } finally {
      fs.statSync = originalStatSync;
    }

    assert.ok(
      sessionStatCalls <= 10,
      `expected active-only stat calls, received ${sessionStatCalls}`,
    );
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});

test("CodexLogMonitor discovers a new session on the fast current-day pass", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "codex-log-monitor-new-"));
  try {
    const root = path.join(home, "sessions");
    const dir = dayDir(root);
    fs.mkdirSync(dir, { recursive: true });
    const events = [];
    const monitor = new CodexLogMonitor({
      SESSION_DIR: root,
      SESSION_INDEX_PATH: "",
      LOOKBACK_DAYS: 1,
      POLL_INTERVAL_MS: 1500,
      DISCOVERY_INTERVAL_MS: 60000,
      NEW_FILE_MAX_AGE_MS: 120000,
      INITIAL_TAIL_BYTES: 0,
      WATCH_FILES: false,
      LOG_EVENT_MAP: {
        "event_msg:agent_message": "speaking",
      },
    }, (sessionId, state, event, extra) => {
      events.push({ sessionId, state, event, extra });
    });
    monitor.baselineExistingSessions();

    const filePath = path.join(dir, rolloutName(123));
    writeJsonl(filePath, [
      {
        type: "event_msg",
        payload: { type: "agent_message", message: "live reply" },
      },
    ]);
    monitor.poll();

    assert.equal(events.length, 1);
    assert.equal(events[0].state, "speaking");
    assert.equal(events[0].event, "event_msg:agent_message");
    assert.equal(events[0].extra.display.content, "live reply");
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});

test("CodexLogMonitor suppresses internal approval review lifecycle events", () => {
  const events = [];
  const monitor = new CodexLogMonitor({
    SESSION_INDEX_PATH: "",
    LOG_EVENT_MAP: {
      "event_msg:task_started": "thinking",
      "event_msg:user_message": "thinking",
      "event_msg:agent_message": "speaking",
      "event_msg:task_complete": "attention",
    },
  }, (sessionId, state, event) => events.push({ sessionId, state, event }));
  const entry = monitor.createEntry("codex:internal-review", 0, false);

  monitor.processLine(JSON.stringify({
    type: "event_msg",
    payload: { type: "task_started", turn_id: "review-turn" },
  }), entry);
  monitor.processLine(JSON.stringify({
    type: "event_msg",
    payload: {
      type: "user_message",
      message: "The following is the Code. Return risk_level, user_authorization, outcome, and rationale.",
    },
  }), entry);
  monitor.processLine(JSON.stringify({
    type: "event_msg",
    payload: { type: "agent_message", message: '{"outcome":"allow"}' },
  }), entry);

  assert.equal(entry.internalSession, true);
  assert.deepEqual(events.map((event) => event.event), ["event_msg:task_started"]);
});

test("CodexLogMonitor restores old rollouts when their source event is current", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "codex-log-monitor-resume-"));
  try {
    const root = path.join(home, "sessions");
    const oldDate = new Date();
    oldDate.setDate(oldDate.getDate() - 90);
    const dir = dayDir(root, oldDate);
    const activePath = path.join(dir, rolloutName(700));
    const terminalPath = path.join(dir, rolloutName(701));
    const currentTimestamp = new Date().toISOString();
    writeJsonl(activePath, [
      { type: "session_meta", payload: { cwd: "D:/old-project" } },
      {
        timestamp: currentTimestamp,
        type: "event_msg",
        payload: { type: "task_started", turn_id: "turn-active" },
      },
    ]);
    writeJsonl(terminalPath, [
      { type: "session_meta", payload: { cwd: "D:/old-project" } },
      {
        timestamp: currentTimestamp,
        type: "event_msg",
        payload: { type: "task_started", turn_id: "turn-done" },
      },
      {
        timestamp: currentTimestamp,
        type: "event_msg",
        payload: { type: "task_complete", turn_id: "turn-done" },
      },
    ]);

    const events = [];
    const monitor = new CodexLogMonitor({
      SESSION_DIR: root,
      SESSION_INDEX_PATH: "",
      LOOKBACK_DAYS: 1,
      POLL_INTERVAL_MS: 1500,
      DISCOVERY_INTERVAL_MS: 60000,
      NEW_FILE_MAX_AGE_MS: 120000,
      INITIAL_TAIL_BYTES: 1024 * 1024,
      WATCH_FILES: false,
      LOG_EVENT_MAP: {
        "event_msg:task_started": "thinking",
        "event_msg:task_complete": "attention",
      },
    }, (sessionId, state, event) => {
      events.push({ sessionId, state, event });
    });

    monitor.baselineExistingSessions();

    assert.deepEqual(events.map((event) => event.state), ["thinking"]);
    assert.equal(events[0].sessionId.endsWith("000000000700"), true);
    assert.equal(events[0].event, "event_msg:task_started");
    assert.equal(monitor.tracked.has(activePath), true);
    assert.equal(monitor.tracked.has(terminalPath), true);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});

test("CodexLogMonitor does not restore stale active events from a freshly touched file", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "codex-log-monitor-stale-"));
  try {
    const root = path.join(home, "sessions");
    const filePath = path.join(dayDir(root), rolloutName(702));
    writeJsonl(filePath, [
      {
        timestamp: new Date(Date.now() - 10 * 60_000).toISOString(),
        type: "event_msg",
        payload: { type: "task_started", turn_id: "stale-turn" },
      },
    ]);
    fs.utimesSync(filePath, new Date(), new Date());

    const events = [];
    const monitor = new CodexLogMonitor({
      SESSION_DIR: root,
      SESSION_INDEX_PATH: "",
      LOOKBACK_DAYS: 1,
      NEW_FILE_MAX_AGE_MS: 120_000,
      STALE_TIMEOUT_MS: 300_000,
      INITIAL_TAIL_BYTES: 1024 * 1024,
      WATCH_FILES: false,
      LOG_EVENT_MAP: {
        "event_msg:task_started": "thinking",
      },
    }, (sessionId, state, event) => {
      events.push({ sessionId, state, event });
    });

    monitor.baselineExistingSessions();

    assert.deepEqual(events, []);
    assert.equal(monitor.tracked.has(filePath), true);
    assert.equal(monitor.tracked.get(filePath).lastState, "thinking");
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});
