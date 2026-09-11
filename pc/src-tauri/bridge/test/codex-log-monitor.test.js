"use strict";

/*
 * [Input] Synthetic Codex rollout directories, Desktop diagnostic rows, and append events.
 * [Output] Regression coverage for low-cost discovery, Desktop fallback recovery, arbitrary-age active recovery, prompt live updates, and current-day Token aggregation.
 * [Pos] Unit tests for the managed bridge's Codex rollout monitor.
 */

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { test } = require("node:test");

const CodexLogMonitor = require("../agents/codex-log-monitor");
const codexDefaults = require("../agents/codex");
const { parseDesktopLogSignal } = CodexLogMonitor;

test("Desktop fallback requires distinct completion flags and never overrides newer or internal rollouts", () => {
  assert.equal(parseDesktopLogSignal({
    thread_id: "test", ts: Date.now() / 1000, target: "codex_core::session::turn",
    feedback_log_body: "post sampling token usage model_needs_follow_up=false has_pending_input=false needs_follow_up=true",
  }), null);
  const events = [];
  const monitor = new CodexLogMonitor({ SESSION_INDEX_PATH: "" }, (...args) => events.push(args));
  monitor.getIndexedTitle = () => "用户任务";
  const entry = monitor.createEntry("codex:test", 0, false);
  entry.lastState = "thinking";
  entry.turnId = "new-turn";
  entry.lastSourceEventTime = Date.now();
  monitor.tracked.set("test", entry);
  const signal = { sessionId: "codex:test", state: "attention",
    event: "event_msg:task_complete", timestampMs: Date.now() - 1000,
    turnId: "old-turn", content: "已完成" };
  monitor.emitDesktopLogSignal(signal);
  entry.internalSession = true;
  monitor.emitDesktopLogSignal({ ...signal, timestampMs: Date.now() + 1000 });
  assert.equal(events.length, 0);
  monitor.getIndexedTitle = () => "";
  monitor.emitDesktopLogSignal({ ...signal, sessionId: "codex:unknown" });
  assert.equal(events.length, 0);
});

let DatabaseSync;
try { ({ DatabaseSync } = require("node:sqlite")); } catch {}
test("Desktop SQLite fallback bootstraps late databases and drains bounded incremental batches", {
  skip: !DatabaseSync,
}, () => {
  const db = new DatabaseSync(":memory:");
  try {
    db.exec("CREATE TABLE logs (id INTEGER PRIMARY KEY, ts INTEGER, thread_id TEXT, target TEXT, feedback_log_body TEXT)");
    const insert = db.prepare("INSERT INTO logs (ts, thread_id, target, feedback_log_body) VALUES (?, ?, ?, ?)");
    const now = Math.floor(Date.now() / 1000);
    insert.run(now - 1000, "old", "codex_core::session::handlers", "op: TurnInput");
    const events = [];
    const monitor = new CodexLogMonitor({ SESSION_INDEX_PATH: "", DESKTOP_LOG_DB: db }, (...args) => events.push(args));
    monitor.getIndexedTitle = () => "用户任务";
    monitor.pollDesktopLogState();
    assert.equal(events.length, 0);
    assert.equal(monitor.desktopLogCursor, 1);
    for (let index = 0; index < 520; index++) {
      insert.run(now, "session-" + index, "codex_core::session::handlers", "op: TurnInput");
    }
    monitor.pollDesktopLogState();
    assert.equal(monitor.desktopLogCursor, 513);
    assert.equal(events.length, 512);
    monitor.pollDesktopLogState();
    assert.equal(monitor.desktopLogCursor, 521);
    assert.equal(events.length, 520);
    assert.equal(monitor.desktopLogEntries.size, 256);
    monitor.pollDesktopLogState();
    assert.equal(events.length, 520);
    db.exec("DELETE FROM logs");
    insert.run(now, "reset", "codex_core::session::handlers", "op: TurnInput");
    monitor.pollDesktopLogState();
    assert.equal(monitor.desktopLogCursor, 1);
    assert.equal(events.at(-1)[0], "codex:reset");
  } finally { db.close(); }
});

test("assistant log mirrors emit once in either order, but repeated turns remain visible", () => {
  for (const order of [["event_msg", "response_item"], ["response_item", "event_msg"]]) {
    const events = [];
    const monitor = new CodexLogMonitor(codexDefaults, (_, state, event, extra) => {
      events.push({ state, event, extra });
    });
    const entry = monitor.createEntry("codex:mirror-test", 0, false);
    const send = (type, message, channel) => monitor.processLine(JSON.stringify({
      type,
      payload: type === "event_msg"
        ? { type: "agent_message", message, channel }
        : { type: "message", role: "assistant", channel,
            content: [{ type: "output_text", text: message }] },
    }), entry);
    for (const type of order) send(type, "正在检查输入", "commentary");
    assert.equal(events.length, 1);
    assert.equal(entry.agentMessageCount, 1);
    for (const type of order) send(type, "不应显示的内容", "analysis");
    assert.equal(events.length, 1);
    monitor.processLine(JSON.stringify({
      type: "event_msg", payload: { type: "task_started", turn_id: "next" },
    }), entry);
    for (const type of order) send(type, "正在检查输入", "commentary");
    assert.equal(entry.agentMessageCount, 2);
    assert.equal(events.filter(({ event }) => event.endsWith("agent_message")
      || event.endsWith("assistant_message")).length, 2);

    // Equal preview prefixes must not collapse different full messages.
    const prefix = "a".repeat(300);
    send("event_msg", prefix + "first", "final");
    send("response_item", prefix + "second", "final");
    assert.equal(entry.agentMessageCount, 4);
    assert.ok(entry.pendingAssistantMessages.length <= 64);
  }
});

test("assistant mirror tracking is bounded and preserves same-source repeated replies", () => {
  const monitor = new CodexLogMonitor(codexDefaults, () => {});
  const entry = monitor.createEntry("codex:bounded-test", 0, false);
  for (let index = 0; index < 100; index++) {
    monitor.processLine(JSON.stringify({
      type: "response_item",
      payload: { type: "message", role: "assistant", channel: "final",
        content: [{ type: "output_text", text: "重复回复" }] },
    }), entry);
  }
  assert.equal(entry.agentMessageCount, 100);
  assert.equal(entry.pendingAssistantMessages.length, 64);
});

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

test("CodexLogMonitor maps Desktop diagnostic lifecycle rows without exposing their body", () => {
  const started = parseDesktopLogSignal({
    ts: 1_789_100_000,
    thread_id: "desktop-session",
    target: "codex_core::session::handlers",
    feedback_log_body: 'session_loop: Submission sub=Submission { id: "turn-live", op: TurnInput { request: secret } }',
  });
  const completed = parseDesktopLogSignal({
    ts: 1_789_100_100,
    thread_id: "desktop-session",
    target: "codex_core::session::turn",
    feedback_log_body: "post sampling token usage model_needs_follow_up=false has_pending_input=false needs_follow_up=false",
  });
  const failed = parseDesktopLogSignal({
    ts: 1_789_100_200,
    thread_id: "desktop-session",
    target: "codex_core::session::turn",
    feedback_log_body: "session_task.run: Turn error: capacity",
  });

  assert.deepEqual(started, {
    sessionId: "codex:desktop-session",
    state: "thinking",
    event: "event_msg:task_started",
    timestampMs: 1_789_100_000_000,
    turnId: "turn-live",
    content: "正在思考",
  });
  assert.equal(completed.state, "attention");
  assert.equal(completed.event, "event_msg:task_complete");
  assert.equal(failed.state, "error");
  assert.equal(failed.content, "执行失败");
  assert.equal(Object.prototype.hasOwnProperty.call(started, "feedback_log_body"), false);
});

test("CodexLogMonitor Desktop fallback emits only a fresh active bootstrap state", () => {
  const events = [];
  const activity = new Map([
    ["codex:fresh-session", Date.now()],
    ["codex:stale-session", Date.now() - 10 * 60_000],
  ]);
  const monitor = new CodexLogMonitor({
    SESSION_INDEX_PATH: "",
    DESKTOP_LOG_DB: {},
  }, (sessionId, state, event, extra) => {
    events.push({ sessionId, state, event, extra });
  });
  monitor.desktopLogActivityMs = (_db, sessionId) => activity.get(sessionId) || 0;
  monitor.getIndexedTitle = () => "用户任务";

  monitor.applyDesktopLogRows([
    {
      ts: Math.floor(Date.now() / 1000) - 10,
      thread_id: "fresh-session",
      target: "codex_core::session::handlers",
      feedback_log_body: 'Submission sub=Submission { id: "turn-fresh", op: TurnInput {} }',
    },
    {
      ts: Math.floor(Date.now() / 1000) - 600,
      thread_id: "stale-session",
      target: "codex_core::session::handlers",
      feedback_log_body: 'Submission sub=Submission { id: "turn-stale", op: TurnInput {} }',
    },
    {
      ts: Math.floor(Date.now() / 1000) - 5,
      thread_id: "done-session",
      target: "codex_core::session::turn",
      feedback_log_body: "post sampling token usage model_needs_follow_up=false has_pending_input=false needs_follow_up=false",
    },
  ], { bootstrap: true, db: {} });

  assert.deepEqual(events.map((event) => [event.sessionId, event.state, event.event]), [[
    "codex:fresh-session",
    "thinking",
    "event_msg:task_started",
  ]]);
  assert.equal(events[0].extra.display.title, "用户任务");
});

test("CodexLogMonitor does not let a rollout start hide a Desktop-only completion", () => {
  const events = [];
  const monitor = new CodexLogMonitor({ SESSION_INDEX_PATH: "" }, (
    sessionId,
    state,
    event,
  ) => events.push({ sessionId, state, event }));
  const rollout = monitor.createEntry("codex:mixed-session", 0, false);
  rollout.lastState = "thinking";
  rollout.lastEvent = "event_msg:task_started";
  rollout.lastSourceEventTime = Date.now();
  monitor.tracked.set("mixed-rollout", rollout);

  monitor.emitDesktopLogSignal({
    sessionId: "codex:mixed-session",
    state: "attention",
    event: "event_msg:task_complete",
    timestampMs: Date.now(),
    turnId: "turn-mixed",
    content: "已完成",
  });

  assert.deepEqual(events, [{
    sessionId: "codex:mixed-session",
    state: "attention",
    event: "event_msg:task_complete",
  }]);
});

test("CodexLogMonitor aggregates today's token totals across sessions", () => {
  const events = [];
  const monitor = new CodexLogMonitor({
    SESSION_DIR: "/tmp/unused-codex-sessions",
    SESSION_INDEX_PATH: "",
    LOG_EVENT_MAP: {},
  }, (sessionId, state, event, extra) => {
    events.push({ sessionId, state, event, extra });
  });
  const first = monitor.createEntry("codex:daily-a", 0, false);
  const second = monitor.createEntry("codex:daily-b", 0, false);
  const timestamp = new Date().toISOString();

  monitor.processLine(JSON.stringify({
    timestamp,
    type: "event_msg",
    payload: {
      type: "token_count",
      info: {
        total_token_usage: {
          total_tokens: 100,
          input_tokens: 70,
          output_tokens: 30,
        },
      },
    },
  }), first);
  monitor.processLine(JSON.stringify({
    timestamp,
    type: "event_msg",
    payload: {
      type: "token_count",
      info: {
        total_token_usage: {
          total_tokens: 250,
          input_tokens: 180,
          output_tokens: 70,
        },
      },
    },
  }), second);

  const daily = events.at(-1).extra.dailyTokenUsage;
  assert.equal(daily.totalTokens, 350);
  assert.equal(daily.inputTokens, 250);
  assert.equal(daily.outputTokens, 100);
});

test("CodexLogMonitor reports only today's delta for a session resumed across midnight", () => {
  const events = [];
  const monitor = new CodexLogMonitor({
    SESSION_DIR: "/tmp/unused-codex-sessions",
    SESSION_INDEX_PATH: "",
    LOG_EVENT_MAP: {},
  }, (_sessionId, _state, _event, extra) => events.push(extra));
  const entry = monitor.createEntry("codex:resumed", 0, false);
  entry.sessionStartDayKey = "2000-01-01";
  entry.latestCumulativeTokenUsage = {
    totalTokens: 1000,
    inputTokens: 800,
    outputTokens: 200,
  };

  monitor.processLine(JSON.stringify({
    timestamp: new Date().toISOString(),
    type: "event_msg",
    payload: {
      type: "token_count",
      info: {
        total_token_usage: {
          total_tokens: 1120,
          input_tokens: 890,
          output_tokens: 230,
        },
      },
    },
  }), entry);

  const daily = events.at(-1).dailyTokenUsage;
  assert.equal(daily.totalTokens, 120);
  assert.equal(daily.inputTokens, 90);
  assert.equal(daily.outputTokens, 30);
});

test("CodexLogMonitor emits a non-visible daily snapshot during startup", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "codex-log-monitor-daily-start-"));
  try {
    const root = path.join(home, "sessions");
    const dir = dayDir(root);
    const filePath = path.join(dir, rolloutName(77));
    const events = [];
    writeJsonl(filePath, [{
      timestamp: new Date().toISOString(),
      type: "event_msg",
      payload: {
        type: "token_count",
        info: {
          total_token_usage: { total_tokens: 42, input_tokens: 30, output_tokens: 12 },
        },
      },
    }]);
    const monitor = new CodexLogMonitor({
      SESSION_DIR: root,
      SESSION_INDEX_PATH: "",
      WATCH_FILES: false,
      NEW_FILE_MAX_AGE_MS: 120000,
    }, (sessionId, state, event, extra) => events.push({ sessionId, state, event, extra }));

    monitor.baselineExistingSessions();

    const snapshot = events.find((event) => event.event === "token_usage:daily_snapshot");
    assert.equal(snapshot.state, "idle");
    assert.equal(snapshot.extra.dailyTokenUsage.totalTokens, 42);
    assert.equal(snapshot.extra.display, undefined);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});

test("CodexLogMonitor finds the midnight baseline outside its bounded tail", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "codex-log-monitor-midnight-"));
  try {
    const root = path.join(home, "sessions");
    const yesterday = new Date();
    yesterday.setDate(yesterday.getDate() - 1);
    const dir = dayDir(root, yesterday);
    const filePath = path.join(dir, rolloutName(78));
    const events = [];
    const oldTimestamp = new Date(Date.now() - 24 * 60 * 60 * 1000).toISOString();
    const timestamp = new Date().toISOString();
    writeJsonl(filePath, [
      {
        timestamp: oldTimestamp,
        type: "event_msg",
        payload: {
          type: "token_count",
          info: {
            total_token_usage: { total_tokens: 1000, input_tokens: 800, output_tokens: 200 },
            last_token_usage: { total_tokens: 100, input_tokens: 80, output_tokens: 20 },
          },
        },
      },
      { timestamp: oldTimestamp, type: "event_msg", payload: { type: "user_message", message: "x".repeat(600_000) } },
      {
        timestamp,
        type: "event_msg",
        payload: {
          type: "token_count",
          info: {
            total_token_usage: { total_tokens: 1120, input_tokens: 890, output_tokens: 230 },
            last_token_usage: { total_tokens: 120, input_tokens: 90, output_tokens: 30 },
          },
        },
      },
      {
        timestamp,
        type: "event_msg",
        payload: {
          type: "token_count",
          info: {
            total_token_usage: { total_tokens: 1175, input_tokens: 930, output_tokens: 245 },
            last_token_usage: { total_tokens: 55, input_tokens: 40, output_tokens: 15 },
          },
        },
      },
    ]);
    const monitor = new CodexLogMonitor({
      SESSION_DIR: root,
      SESSION_INDEX_PATH: "",
      WATCH_FILES: false,
      NEW_FILE_MAX_AGE_MS: 120000,
    }, (_sessionId, _state, event, extra) => {
      if (event === "token_usage:daily_snapshot") events.push(extra);
    });

    monitor.baselineExistingSessions();

    const daily = events.at(-1).dailyTokenUsage;
    assert.equal(daily.totalTokens, 175);
    assert.equal(daily.inputTokens, 130);
    assert.equal(daily.outputTokens, 45);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
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

test("CodexLogMonitor preserves current reply through tools and thinking, then clears it on a new turn", () => {
  const events = [];
  const monitor = new CodexLogMonitor({
    SESSION_INDEX_PATH: "",
    LOG_EVENT_MAP: {
      "event_msg:task_started": "thinking",
      "event_msg:agent_reasoning": "thinking",
      "response_item:custom_tool_call": "working",
      "response_item:web_search_call": "working",
    },
  }, (_sessionId, state, event, extra) => events.push({ state, event, extra }));
  const entry = monitor.createEntry("codex:live-context", 0, false);
  entry.sessionTitle = "修复实时上下文";

  monitor.processLine(JSON.stringify({
    type: "event_msg",
    payload: { type: "task_started", turn_id: "turn-live" },
  }), entry);
  monitor.processLine(JSON.stringify({
    type: "response_item",
    payload: {
      type: "message",
      role: "assistant",
      content: [{ type: "output_text", text: "正在定位状态更新链路。" }],
    },
  }), entry);
  monitor.processLine(JSON.stringify({
    type: "response_item",
    payload: { type: "custom_tool_call", name: "functions.exec", input: "..." },
  }), entry);
  monitor.processLine(JSON.stringify({
    type: "response_item",
    payload: { type: "web_search_call", action: { query: "Codex events" } },
  }), entry);

  assert.deepEqual(events.map((item) => item.event), [
    "event_msg:task_started",
    "response_item:assistant_message",
    "response_item:custom_tool_call",
    "response_item:web_search_call",
  ]);
  assert.equal(events[1].state, "speaking");
  assert.equal(events[1].extra.display.content, "正在定位状态更新链路。");
  assert.equal(events[2].extra.display.content, "正在定位状态更新链路。");
  assert.equal(events[3].extra.display.content, "正在定位状态更新链路。");

  monitor.processLine(JSON.stringify({
    type: "event_msg", payload: { type: "agent_reasoning", text: "private reasoning" },
  }), entry);
  assert.equal(events.at(-1).extra.display.content, "正在定位状态更新链路。");
  monitor.processLine(JSON.stringify({
    type: "response_item",
    payload: { type: "message", role: "assistant", channel: "analysis",
      content: [{ type: "output_text", text: "private reasoning" }] },
  }), entry);
  assert.equal(entry.lastAgentMessage, "正在定位状态更新链路。");
  monitor.processLine(JSON.stringify({
    type: "event_msg", payload: { type: "task_started", turn_id: "next-turn" },
  }), entry);
  assert.equal(events.at(-1).extra.display.content, "正在思考");
  monitor.processLine(JSON.stringify({
    type: "response_item", payload: { type: "custom_tool_call", name: "functions.exec" },
  }), entry);
  assert.equal(events.at(-1).extra.display.content, "正在运行命令");
  assert.equal(entry.lastTaskCompleteMessage, "");
});

test("CodexLogMonitor attributes resumed writer rollouts to the original thread", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "codex-writer-identity-"));
  try {
    const root = path.join(home, "sessions");
    const threadId = "00000000-0000-4000-8000-000000000123";
    const writerId = "00000000-0000-4000-8000-000000000456";
    const fileName = `rollout-2026-09-11T10-00-00-${threadId}_${writerId}.jsonl`;
    const filePath = path.join(dayDir(root), fileName);
    const events = [];
    const monitor = new CodexLogMonitor({
      SESSION_DIR: root, SESSION_INDEX_PATH: "", WATCH_FILES: false,
    }, (sessionId, state, event, extra) => events.push({ sessionId, state, extra }));
    assert.equal(monitor.extractSessionId(fileName), `codex:${threadId}`);
    assert.equal(monitor.extractSessionId("rollout-bad-name.jsonl"), null);
    writeJsonl(filePath, [
      { type: "event_msg", payload: { type: "task_started", turn_id: "live-turn" } },
      { type: "response_item", payload: { type: "message", role: "assistant",
        channel: "commentary", content: [{ type: "output_text", text: "Current reply" }] } },
      { type: "response_item", payload: { type: "function_call", name: "exec_command" } },
    ]);
    monitor.pollFile(filePath, fileName);
    assert.ok(events.length >= 2);
    assert.ok(events.every((event) => event.sessionId === `codex:${threadId}`));
    assert.equal(events.at(-1).extra.display.content, "Current reply");
    const entry = monitor.tracked.get(filePath);
    entry.lastSourceEventTime = Date.now();
    assert.equal(monitor.hasRecentRolloutState({ sessionId: `codex:${threadId}`,
      event: "event_msg:task_started", timestampMs: Date.now() }), true);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
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
