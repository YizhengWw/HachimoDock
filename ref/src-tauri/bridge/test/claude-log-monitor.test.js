"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { test } = require("node:test");

const ClaudeLogMonitor = require("../agents/claude-log-monitor");

function writeJsonl(filePath, lines) {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  fs.writeFileSync(filePath, `${lines.map((line) => JSON.stringify(line)).join("\n")}\n`);
}

test("ClaudeLogMonitor emits completed display payloads per session file", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "claude-log-monitor-"));
  try {
    const root = path.join(home, ".claude", "projects");
    const first = path.join(root, "-tmp-one", "session-one.jsonl");
    const second = path.join(root, "-tmp-two", "session-two.jsonl");

    writeJsonl(first, [
      {
        type: "user",
        sessionId: "session-one",
        cwd: "/tmp/one",
        message: { role: "user", content: "请修复登录报错" },
      },
      {
        type: "assistant",
        sessionId: "session-one",
        cwd: "/tmp/one",
        message: {
          role: "assistant",
          content: [{ type: "text", text: "登录报错已经修复，并补了回归测试。" }],
          usage: { input_tokens: 12, output_tokens: 34 },
        },
      },
    ]);

    writeJsonl(second, [
      {
        type: "user",
        sessionId: "session-two",
        cwd: "/tmp/two",
        message: { role: "user", content: "优化设备状态展示" },
      },
      {
        type: "assistant",
        sessionId: "session-two",
        cwd: "/tmp/two",
        message: {
          role: "assistant",
          content: [{ type: "text", text: "设备状态展示已支持多个完成结果。" }],
        },
      },
    ]);

    const events = [];
    const monitor = new ClaudeLogMonitor({
      PROJECTS_ROOT: root,
      INITIAL_TAIL_BYTES: 0,
      HEARTBEAT_MS: 3600000,
      PROCESS_NAMES_UNIX: [],
      PROCESS_NAMES_WIN: [],
    }, (sessionId, state, event, extra) => {
      events.push({ sessionId, state, event, extra });
    });

    monitor.pollSessions();

    const doneEvents = events.filter((item) => item.event === "claude:assistant_message");
    assert.equal(doneEvents.length, 2);
    assert.deepEqual(doneEvents.map((item) => item.sessionId).sort(), ["session-one", "session-two"]);
    assert.equal(doneEvents.find((item) => item.sessionId === "session-one").state, "done");
    assert.match(doneEvents.find((item) => item.sessionId === "session-one").extra.display.content, /登录报错已经修复/);
    assert.equal(doneEvents.find((item) => item.sessionId === "session-one").extra.tokenUsage.totalTokens, 46);
    assert.match(doneEvents.find((item) => item.sessionId === "session-two").extra.display.content, /多个完成结果/);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});

test("ClaudeLogMonitor keeps tool-use assistant steps working, only terminal text is done", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "claude-log-monitor-tooluse-"));
  try {
    const root = path.join(home, ".claude", "projects");
    const file = path.join(root, "-tmp-agentic", "session-agentic.jsonl");

    writeJsonl(file, [
      {
        type: "user",
        sessionId: "session-agentic",
        cwd: "/tmp/agentic",
        message: { role: "user", content: "重构这个文件" },
      },
      // Mid-turn tool call: the agent is still working, NOT done. (Both the
      // content tool_use block and stop_reason="tool_use" mark it.)
      {
        type: "assistant",
        sessionId: "session-agentic",
        cwd: "/tmp/agentic",
        message: {
          role: "assistant",
          stop_reason: "tool_use",
          content: [
            { type: "text", text: "我先看一下文件。" },
            { type: "tool_use", id: "toolu_1", name: "Read", input: { file: "x" } },
          ],
        },
      },
      // Tool result arrives as a user-role line (already maps to working).
      {
        type: "user",
        sessionId: "session-agentic",
        cwd: "/tmp/agentic",
        message: { role: "user", content: [{ type: "tool_result", tool_use_id: "toolu_1", content: "ok" }] },
      },
      // Terminal assistant text (no tool_use): the turn really finished.
      {
        type: "assistant",
        sessionId: "session-agentic",
        cwd: "/tmp/agentic",
        message: {
          role: "assistant",
          stop_reason: "end_turn",
          content: [{ type: "text", text: "重构完成。" }],
        },
      },
    ]);

    const events = [];
    const monitor = new ClaudeLogMonitor({
      PROJECTS_ROOT: root,
      INITIAL_TAIL_BYTES: 0,
      HEARTBEAT_MS: 3600000,
      PROCESS_NAMES_UNIX: [],
      PROCESS_NAMES_WIN: [],
    }, (sessionId, state, event, extra) => {
      events.push({ sessionId, state, event, extra });
    });

    monitor.pollSessions();

    // The tool-use step must NOT be reported as done — it stays working.
    const toolUse = events.filter((e) => e.event === "claude:tool_use");
    assert.equal(toolUse.length, 1, "tool-use assistant line should emit a working event");
    assert.equal(toolUse[0].state, "working");

    // Only the terminal text assistant line is "done".
    const done = events.filter((e) => e.event === "claude:assistant_message");
    assert.equal(done.length, 1, "only the terminal assistant text should be done");
    assert.equal(done[0].state, "done");

    // Sanity: no event in the whole turn reported state "done" except the terminal one,
    // and the last emitted state is the terminal done.
    assert.equal(events.filter((e) => e.state === "done").length, 1);
    assert.equal(events[events.length - 1].state, "done");
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});

test("ClaudeLogMonitor baselines old sessions before reading appended turns", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "claude-log-monitor-baseline-"));
  try {
    const root = path.join(home, ".claude", "projects");
    const file = path.join(root, "-tmp-reused", "session-reused.jsonl");

    writeJsonl(file, [
      {
        type: "user",
        sessionId: "session-reused",
        cwd: "/tmp/reused",
        message: { role: "user", content: "之前的旧任务" },
      },
      {
        type: "assistant",
        sessionId: "session-reused",
        cwd: "/tmp/reused",
        message: {
          role: "assistant",
          stop_reason: "end_turn",
          content: [{ type: "text", text: "旧任务已经完成。" }],
        },
      },
    ]);

    const oldTime = new Date(Date.now() - 10_000);
    fs.utimesSync(file, oldTime, oldTime);

    const events = [];
    const monitor = new ClaudeLogMonitor({
      PROJECTS_ROOT: root,
      INITIAL_TAIL_BYTES: 512,
      NEW_FILE_MAX_AGE_MS: 1000,
      HEARTBEAT_MS: 3600000,
      PROCESS_NAMES_UNIX: [],
      PROCESS_NAMES_WIN: [],
    }, (sessionId, state, event, extra) => {
      events.push({ sessionId, state, event, extra });
    });

    monitor.pollSessions();

    assert.equal(events.length, 0, "old sessions should not replay historical state");
    assert.equal(monitor.tracked.has(file), true, "old sessions should still be tracked from their current size");

    fs.appendFileSync(file, `${JSON.stringify({
      type: "user",
      sessionId: "session-reused",
      cwd: "/tmp/reused",
      message: { role: "user", content: "继续处理这个任务" },
    })}\n`);
    fs.appendFileSync(file, `${JSON.stringify({
      type: "assistant",
      sessionId: "session-reused",
      cwd: "/tmp/reused",
      message: {
        role: "assistant",
        stop_reason: "tool_use",
        content: [
          { type: "text", text: "我先读取上下文。" },
          { type: "tool_use", id: "toolu_2", name: "Read", input: { file: "large.log" } },
        ],
      },
    })}\n`);
    fs.appendFileSync(file, `${JSON.stringify({
      type: "user",
      sessionId: "session-reused",
      cwd: "/tmp/reused",
      message: {
        role: "user",
        content: [{ type: "tool_result", tool_use_id: "toolu_2", content: "x".repeat(4096) }],
      },
    })}\n`);
    fs.appendFileSync(file, `${JSON.stringify({
      type: "assistant",
      sessionId: "session-reused",
      cwd: "/tmp/reused",
      message: {
        role: "assistant",
        stop_reason: "end_turn",
        content: [{ type: "text", text: "继续任务已经完成。" }],
      },
    })}\n`);

    monitor.pollSessions();

    assert.equal(events[0].state, "working");
    assert.equal(events[0].event, "claude:user_message");
    assert.equal(events.some((item) => item.event === "claude:tool_use" && item.state === "working"), true);
    assert.equal(events[events.length - 1].state, "done");
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});
