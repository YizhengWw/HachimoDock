"use strict";

/*
 * [Input] Mock Agent adapters, hook-backed session status fixtures, and HTTP requests.
 * [Output] Agent Session Bus route coverage for cursor lifecycle events, desktop-only discovery, duplicate rejection, and terminal-stream cleanup.
 * [Pos] HTTP integration tests for Agent Session Bus.
 */

const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const http = require("node:http");
const os = require("node:os");
const path = require("node:path");

const { createAgentSessionBus, MockAdapter } = require("../src/index");
const { listClaudeSessions } = require("../src/util/claude-paths");

function silentLog() { return () => {}; }

function jsonRequest(port, method, pathname, body) {
  return new Promise((resolve, reject) => {
    const req = http.request(
      { host: "127.0.0.1", port, method, path: pathname, headers: { "Content-Type": "application/json" } },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          const raw = Buffer.concat(chunks).toString("utf8");
          let parsed = null;
          try { parsed = JSON.parse(raw); } catch { /* keep raw */ }
          resolve({ status: res.statusCode, body: parsed, raw });
        });
      },
    );
    req.on("error", reject);
    if (body !== undefined) req.write(typeof body === "string" ? body : JSON.stringify(body));
    req.end();
  });
}

function sseRequest(port, method, pathname, body) {
  return new Promise((resolve, reject) => {
    const req = http.request(
      { host: "127.0.0.1", port, method, path: pathname, headers: { "Content-Type": "application/json", Accept: "text/event-stream" } },
      (res) => {
        const events = [];
        let buffer = "";
        res.on("data", (chunk) => {
          buffer += chunk.toString("utf8");
          let idx;
          while ((idx = buffer.indexOf("\n\n")) >= 0) {
            const block = buffer.slice(0, idx);
            buffer = buffer.slice(idx + 2);
            const lines = block.split(/\r?\n/);
            const evt = { event: null, data: [] };
            for (const line of lines) {
              if (line.startsWith(": ")) continue;
              if (line.startsWith("event: ")) evt.event = line.slice(7);
              else if (line.startsWith("data: ")) evt.data.push(line.slice(6));
            }
            if (evt.event) {
              try { events.push({ event: evt.event, data: JSON.parse(evt.data.join("\n")) }); }
              catch { events.push({ event: evt.event, data: evt.data.join("\n") }); }
            }
          }
        });
        res.on("end", () => resolve({ status: res.statusCode, events }));
      },
    );
    req.on("error", reject);
    if (body !== undefined) req.write(JSON.stringify(body));
    req.end();
  });
}

async function startBus({ sessionStatusProvider, sessionEventProvider, agentId = "mock", adapter } = {}) {
  const bus = createAgentSessionBus({
    port: 0, // pick free port
    adapters: [adapter || new MockAdapter({ agentId, tokensPerSecond: 60 })],
    sessionStatusProvider,
    sessionEventProvider,
    log: silentLog(),
  });
  const port = await bus.start();
  return { bus, port };
}

test("GET /agent/health works", async () => {
  const { bus, port } = await startBus();
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/health");
    assert.equal(status, 200);
    assert.equal(body.ok, true);
    assert.deepEqual(body.adapters, ["mock"]);
  } finally {
    await bus.stop();
  }
});

test("GET /agent/status reports ready adapter", async () => {
  const { bus, port } = await startBus();
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/status");
    assert.equal(status, 200);
    assert.equal(body.adapters[0].ready, true);
    assert.equal(body.adapters[0].agentId, "mock");
  } finally {
    await bus.stop();
  }
});

test("GET /agent/session-events returns ordered cursor updates", async () => {
  let received = null;
  const { bus, port } = await startBus({
    sessionEventProvider: (agentId, options) => {
      received = { agentId, options };
      return {
        streamId: "stream-a",
        cursor: 9,
        reset: false,
        events: [
          {
            cursor: 8,
            state: "working",
            session: { id: "mock:fast-session", state: "working", statusUpdatedAt: 100 },
          },
          {
            cursor: 9,
            state: "done",
            session: { id: "mock:fast-session", state: "done", statusUpdatedAt: 150 },
          },
        ],
      };
    },
  });
  try {
    const { status, body } = await jsonRequest(
      port,
      "GET",
      "/agent/session-events?agentId=mock&cursor=7&streamId=stream-a&limit=20",
    );
    assert.equal(status, 200);
    assert.equal(body.streamId, "stream-a");
    assert.equal(body.cursor, 9);
    assert.deepEqual(body.events.map((event) => event.state), ["working", "done"]);
    assert.deepEqual(body.events.map((event) => event.session.id), ["fast-session", "fast-session"]);
    assert.deepEqual(received, {
      agentId: "mock",
      options: { cursor: 7, streamId: "stream-a", limit: 20 },
    });
  } finally {
    await bus.stop();
  }
});

test("GET /agent/session-events filters internal Codex approval reviews", async () => {
  const { bus, port } = await startBus({
    agentId: "codex",
    sessionEventProvider: () => ({
      streamId: "codex-stream",
      cursor: 2,
      events: [
        {
          cursor: 1,
          session: {
            id: "codex:internal-review",
            state: "working",
            displayTitle: "The following is the Code",
            displayContent: '{"risk_level":"medium","user_authorization":"high","outcome":"allow","rationale":"ok"}',
          },
        },
        {
          cursor: 2,
          session: {
            id: "codex:user-session",
            state: "working",
            displayTitle: "Build the pet UI",
          },
        },
      ],
    }),
  });
  try {
    const { status, body } = await jsonRequest(
      port,
      "GET",
      "/agent/session-events?agentId=codex",
    );
    assert.equal(status, 200);
    assert.deepEqual(body.events.map((event) => event.session.id), ["user-session"]);
  } finally {
    await bus.stop();
  }
});

test("GET /agent/session-events validates agentId and cursor", async () => {
  const { bus, port } = await startBus();
  try {
    const missingAgent = await jsonRequest(port, "GET", "/agent/session-events");
    assert.equal(missingAgent.status, 400);
    const invalidCursor = await jsonRequest(
      port,
      "GET",
      "/agent/session-events?agentId=mock&cursor=invalid",
    );
    assert.equal(invalidCursor.status, 400);
  } finally {
    await bus.stop();
  }
});
test("GET /agent/sessions requires agentId", async () => {
  const { bus, port } = await startBus();
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions");
    assert.equal(status, 400);
    assert.equal(body.ok, false);
  } finally {
    await bus.stop();
  }
});

test("GET /agent/sessions returns sessions for known agent", async () => {
  const { bus, port } = await startBus();
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=mock&limit=5");
    assert.equal(status, 200);
    assert.equal(body.ok, true);
    assert.equal(body.agentId, "mock");
    assert.ok(Array.isArray(body.sessions));
    assert.ok(body.sessions.length >= 1);
  } finally {
    await bus.stop();
  }
});

test("GET /agent/sessions scans Claude transcripts without an installed CLI", async () => {
  const adapter = new MockAdapter({ agentId: "claude-code", tokensPerSecond: 60 });
  adapter.isAvailable = async () => ({ ready: false, reason: "CLI unavailable" });
  adapter.listSessions = async () => [{
    id: "desktop-transcript-001",
    name: "恢复的桌面会话",
    summary: "恢复的桌面会话",
    lastModified: 45678,
  }];
  const { bus, port } = await startBus({ adapter });
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=claude-code&limit=5");
    assert.equal(status, 200);
    assert.equal(body.ready, true);
    assert.equal(body.sessions[0].id, "desktop-transcript-001");
  } finally {
    await bus.stop();
  }
});

test("Claude transcript discovery marks sessions registered by Claude Desktop", () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "claude-desktop-sessions-"));
  try {
    const projectDir = path.join(root, ".claude", "projects", "-tmp-project");
    const desktopDir = path.join(root, "desktop-sessions", "org", "project");
    fs.mkdirSync(projectDir, { recursive: true });
    fs.mkdirSync(desktopDir, { recursive: true });
    fs.writeFileSync(
      path.join(projectDir, "11111111-1111-4111-8111-111111111111.jsonl"),
      `${JSON.stringify({ cwd: "/tmp/project", type: "user", message: { content: "Desktop task" } })}\n`,
    );
    fs.writeFileSync(
      path.join(projectDir, "22222222-2222-4222-8222-222222222222.jsonl"),
      `${JSON.stringify({ cwd: "/tmp/project", type: "user", message: { content: "CLI-only task" } })}\n`,
    );
    fs.writeFileSync(
      path.join(desktopDir, "local_11111111-1111-4111-8111-111111111111.json"),
      JSON.stringify({
        sessionId: "local_11111111-1111-4111-8111-111111111111",
        cliSessionId: "11111111-1111-4111-8111-111111111111",
      }),
    );

    const sessions = listClaudeSessions({
      env: {
        HOME: root,
        CLAUDE_DESKTOP_SESSIONS_DIR: path.join(root, "desktop-sessions"),
      },
    });
    assert.equal(
      sessions.find((session) => session.id.startsWith("11111111"))?.surface,
      "desktop",
    );
    assert.equal(
      sessions.find((session) => session.id.startsWith("22222222"))?.surface,
      undefined,
    );
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("GET /agent/sessions excludes CLI-only history from a Desktop queue", async () => {
  const adapter = new MockAdapter({ agentId: "claude-code", tokensPerSecond: 60 });
  adapter.listSessions = async () => [
    {
      id: "desktop-session-registered",
      summary: "Desktop task",
      lastModified: 200,
      surface: "desktop",
    },
    {
      id: "cli-session-only",
      summary: "CLI-only task",
      lastModified: 300,
    },
  ];
  const { bus, port } = await startBus({ adapter });
  try {
    const { body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=claude-code&limit=5");
    assert.deepEqual(body.sessions.map((session) => session.id), ["desktop-session-registered"]);
  } finally {
    await bus.stop();
  }
});

test("GET /agent/sessions merges live conversation state", async () => {
  const { bus, port } = await startBus({
    sessionStatusProvider: () => [{
      id: "mock:mock-session-001",
      state: "working",
      updatedAt: 12345,
      displayTitle: "任务标题",
      displayContent: "正在执行设备验证",
    }],
  });
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=mock&limit=5");
    assert.equal(status, 200);
    const session = body.sessions.find((item) => item.id === "mock-session-001");
    assert.equal(session.state, "working");
    assert.equal(session.statusUpdatedAt, 12345);
    assert.equal(session.displayTitle, "任务标题");
    assert.equal(session.displayContent, "正在执行设备验证");
  } finally {
    await bus.stop();
  }
});

test("GET /agent/sessions includes hook-only desktop sessions", async () => {
  const { bus, port } = await startBus({
    agentId: "claude-code",
    sessionStatusProvider: () => [{
      id: "claude-code:desktop-session-002",
      state: "done",
      updatedAt: 23456,
      title: "Claude Desktop 任务",
      cwd: "D:/code/project",
      transcriptPath: "D:/claude/transcript.jsonl",
      displayTitle: "Claude Desktop 任务",
      displayContent: "已完成",
    }],
  });
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=claude-code&limit=5");
    assert.equal(status, 200);
    const session = body.sessions.find((item) => item.id === "desktop-session-002");
    assert.equal(session.surface, "desktop");
    assert.equal(session.summary, "Claude Desktop 任务");
    assert.equal(session.cwd, "D:/code/project");
    assert.equal(session.displayContent, "已完成");
  } finally {
    await bus.stop();
  }
});

test("GET /agent/sessions never pairs a terminal state with active-only card text", async () => {
  const { bus, port } = await startBus({
    agentId: "claude-code",
    sessionStatusProvider: () => [{
      id: "claude-code:desktop-session-terminal",
      state: "done",
      updatedAt: 24000,
      title: "已经完成的任务",
      displayTitle: "已经完成的任务",
      displayContent: "正在处理",
    }],
  });
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=claude-code&limit=5");
    assert.equal(status, 200);
    const session = body.sessions.find((item) => item.id === "desktop-session-terminal");
    assert.equal(session.state, "done");
    assert.equal(session.displayContent, "已完成");
  } finally {
    await bus.stop();
  }
});

test("GET /agent/sessions omits empty Claude lifecycle-only sessions", async () => {
  const { bus, port } = await startBus({
    agentId: "claude-code",
    sessionStatusProvider: () => [
      {
        id: "claude-code:empty-session-start",
        state: "idle",
        updatedAt: 30001,
        cwd: "D:/code/project",
      },
      {
        id: "claude-code:active-hook-session",
        state: "working",
        updatedAt: 30002,
        displayContent: "正在处理",
      },
    ],
  });
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=claude-code&limit=5");
    assert.equal(status, 200);
    assert.equal(body.sessions.some((item) => item.id === "empty-session-start"), false);
    assert.equal(body.sessions.some((item) => item.id === "active-hook-session"), true);
  } finally {
    await bus.stop();
  }
});

test("GET /agent/sessions drops Claude process placeholder when real sessions exist", async () => {
  const { bus, port } = await startBus({
    agentId: "claude-code",
    sessionStatusProvider: () => [
      {
        id: "claude-code:claude:local",
        state: "idle",
        updatedAt: 99999,
        title: "Claude 会话",
      },
      {
        id: "claude-code:desktop-session-003",
        state: "done",
        updatedAt: 90000,
        title: "真实 Claude Desktop 会话",
      },
    ],
  });
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=claude-code&limit=5");
    assert.equal(status, 200);
    assert.equal(body.sessions.some((item) => item.id === "claude:local"), false);
    assert.equal(body.sessions.some((item) => item.id === "desktop-session-003"), true);
  } finally {
    await bus.stop();
  }
});

test("GET /agent/sessions never appends Codex internal status-only sessions", async () => {
  const { bus, port } = await startBus({
    agentId: "codex",
    sessionStatusProvider: () => [{
      id: "codex:internal-turn-002",
      state: "done",
      updatedAt: 34567,
      displayTitle: "Codex 内部状态",
      displayContent: "不应成为设备会话",
    }],
  });
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=codex&limit=5");
    assert.equal(status, 200);
    assert.equal(body.sessions.some((item) => item.id === "internal-turn-002"), false);
    assert.equal(body.sessions.some((item) => item.id === "codex:internal-turn-002"), false);
  } finally {
    await bus.stop();
  }
});

test("GET /agent/sessions excludes active Codex approval review cards", async () => {
  const { bus, port } = await startBus({
    agentId: "codex",
    sessionStatusProvider: () => [{
      id: "codex:approval-review",
      state: "working",
      updatedAt: 44567,
      title: "The following is the Code",
      displayTitle: "The following is the Code",
      displayContent: '{"risk_level":"medium","user_authorization":"high","outcome":"allow","rationale":"ok"}',
    }],
  });
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=codex&limit=5");
    assert.equal(status, 200);
    assert.equal(body.sessions.some((session) => session.id === "approval-review"), false);
    assert.equal(body.sessions.length, 2);
  } finally {
    await bus.stop();
  }
});

test("GET /agent/sessions appends an unseen titled active Codex session", async () => {
  const { bus, port } = await startBus({
    agentId: "codex",
    sessionStatusProvider: () => [{
      id: "codex:active-desktop-session",
      state: "working",
      updatedAt: 45678,
      title: "app development",
      displayTitle: "Codex 会话",
      displayContent: "working",
      cwd: "D:/code/project",
    }],
  });
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=codex&limit=5");
    assert.equal(status, 200);
    const session = body.sessions.find((item) => item.id === "active-desktop-session");
    assert.equal(session.state, "working");
    assert.equal(session.displayTitle, "app development");
    assert.equal(session.cwd, "D:/code/project");
  } finally {
    await bus.stop();
  }
});

test("GET /agent/sessions waits for a real title before appending an active Codex session", async () => {
  const { bus, port } = await startBus({
    agentId: "codex",
    sessionStatusProvider: () => [{
      id: "codex:title-pending-session",
      state: "working",
      updatedAt: 56789,
      displayTitle: "Codex 会话",
      displayContent: "正在处理",
    }],
  });
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/sessions?agentId=codex&limit=5");
    assert.equal(status, 200);
    assert.equal(body.sessions.some((item) => item.id === "title-pending-session"), false);
  } finally {
    await bus.stop();
  }
});

test("POST /agent/inject streams ready+token+done events", async () => {
  const { bus, port } = await startBus();
  try {
    const { status, events } = await sseRequest(port, "POST", "/agent/inject", {
      agentId: "mock",
      sessionId: "auto",
      text: "测试",
    });
    assert.equal(status, 200);
    const kinds = events.map((e) => e.event);
    assert.equal(kinds[0], "ready");
    assert.ok(kinds.includes("token"));
    assert.equal(kinds[kinds.length - 1], "done");
    const ready = events[0].data;
    assert.equal(ready.agentId, "mock");
    assert.equal(typeof ready.runId, "string");
    assert.equal(typeof ready.sessionId, "string");
    const done = events[events.length - 1].data;
    assert.equal(typeof done.sessionId, "string");
  } finally {
    await bus.stop();
  }
});

test("POST /agent/inject drains adapter cleanup after a terminal event", async () => {
  const adapter = new MockAdapter({ agentId: "mock", tokensPerSecond: 240 });
  let cleanupFinished = false;
  adapter.inject = async function* injectWithCleanup() {
    yield { kind: "done", sessionId: "cleanup-session", stopReason: "end_turn" };
    await new Promise((resolve) => setTimeout(resolve, 25));
    cleanupFinished = true;
  };

  const { bus, port } = await startBus({ adapter });
  try {
    const { status, events } = await sseRequest(port, "POST", "/agent/inject", {
      agentId: "mock",
      sessionId: "auto",
      text: "cleanup",
    });
    assert.equal(status, 200);
    assert.equal(events.filter((event) => event.event === "done").length, 1);
    assert.equal(cleanupFinished, true);
  } finally {
    await bus.stop();
  }
});

test('POST /agent/inject sessionId="new" forces openNew (skips resolveActive)', async () => {
  // Voice path contract: BusLLM sends sessionId="new" on the first
  // inject of every voice session (and after barge-in resets). The
  // bus must NOT resume any pre-existing session for that agent —
  // it must mint a fresh one. We pin the *resolveActive* return to
  // a known sid via a stub on the adapter; the test passes only if
  // the bus reports `opened:true` and a different sid on `ready`.
  const adapter = new MockAdapter({ agentId: "mock", tokensPerSecond: 240 });
  const PINNED = "mock-pinned-active-sid";
  // Force resolveActive to claim the pinned sid is the most recent —
  // if the server walks resolveActive on a `new` request, the test
  // will see `sessionId === PINNED` on ready and fail.
  adapter.resolveActive = async () => ({ id: PINNED, lastModified: Date.now() });

  const bus = createAgentSessionBus({ port: 0, adapters: [adapter], log: silentLog() });
  const port = await bus.start();
  try {
    const { status, events } = await sseRequest(port, "POST", "/agent/inject", {
      agentId: "mock",
      sessionId: "new",
      text: "hi",
    });
    assert.equal(status, 200);
    const ready = events.find((e) => e.event === "ready");
    assert.ok(ready, "missing ready event");
    assert.equal(ready.data.opened, true, "ready.opened must be true for sessionId=new");
    assert.notEqual(
      ready.data.sessionId,
      PINNED,
      "sessionId=new must mint a fresh session, not resume resolveActive's choice",
    );
  } finally {
    await bus.stop();
  }
});

test('POST /agent/inject sessionId="auto" still resumes resolveActive', async () => {
  // Regression guard: the new `sessionId="new"` branch must not break
  // the legacy `sessionId="auto"` resume-or-open path that direct
  // curl / REPL callers depend on.
  const adapter = new MockAdapter({ agentId: "mock", tokensPerSecond: 240 });
  const PINNED = "mock-pinned-active-sid";
  adapter.resolveActive = async () => ({ id: PINNED, lastModified: Date.now() });

  const bus = createAgentSessionBus({ port: 0, adapters: [adapter], log: silentLog() });
  const port = await bus.start();
  try {
    const { status, events } = await sseRequest(port, "POST", "/agent/inject", {
      agentId: "mock",
      sessionId: "auto",
      text: "hi",
    });
    assert.equal(status, 200);
    const ready = events.find((e) => e.event === "ready");
    assert.ok(ready);
    assert.equal(ready.data.sessionId, PINNED, 'sessionId="auto" must resume newest');
    assert.equal(ready.data.opened, false);
  } finally {
    await bus.stop();
  }
});

test("POST /agent/inject 404 on unknown agentId", async () => {
  const { bus, port } = await startBus();
  try {
    const { status, body } = await jsonRequest(port, "POST", "/agent/inject", {
      agentId: "nope",
      text: "hi",
    });
    assert.equal(status, 404);
    assert.equal(body.ok, false);
  } finally {
    await bus.stop();
  }
});

test("POST /agent/inject 400 on empty text", async () => {
  const { bus, port } = await startBus();
  try {
    const { status, body } = await jsonRequest(port, "POST", "/agent/inject", {
      agentId: "mock",
      text: "   ",
    });
    assert.equal(status, 400);
    assert.equal(body.ok, false);
  } finally {
    await bus.stop();
  }
});

test("POST /agent/inject 400 on bad JSON", async () => {
  const { bus, port } = await startBus();
  try {
    const { status } = await jsonRequest(port, "POST", "/agent/inject", "{not json");
    assert.equal(status, 400);
  } finally {
    await bus.stop();
  }
});

test("404 on unknown route", async () => {
  const { bus, port } = await startBus();
  try {
    const { status } = await jsonRequest(port, "GET", "/does/not/exist");
    assert.equal(status, 404);
  } finally {
    await bus.stop();
  }
});

// CORS: the Tauri webview at `tauri://localhost` must be able to fetch the
// loopback bus. Without these headers WebKit silently drops the response and
// the voice-card UI gets stuck on "正在检测...".
function rawRequest(port, method, pathname, headers = {}, body) {
  return new Promise((resolve, reject) => {
    const req = http.request(
      { host: "127.0.0.1", port, method, path: pathname, headers },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => resolve({ status: res.statusCode, headers: res.headers, raw: Buffer.concat(chunks).toString("utf8") }));
      },
    );
    req.on("error", reject);
    if (body !== undefined) req.write(typeof body === "string" ? body : JSON.stringify(body));
    req.end();
  });
}

test("OPTIONS preflight returns CORS headers", async () => {
  const { bus, port } = await startBus();
  try {
    const { status, headers } = await rawRequest(port, "OPTIONS", "/agent/inject", {
      Origin: "tauri://localhost",
      "Access-Control-Request-Method": "POST",
      "Access-Control-Request-Headers": "content-type",
    });
    assert.equal(status, 204);
    assert.equal(headers["access-control-allow-origin"], "tauri://localhost");
    assert.match(headers["access-control-allow-methods"] || "", /POST/);
    assert.match(headers["access-control-allow-headers"] || "", /content-type/i);
  } finally {
    await bus.stop();
  }
});

test("GET /agent/status allows the Tauri Origin", async () => {
  const { bus, port } = await startBus();
  try {
    const { status, headers } = await rawRequest(port, "GET", "/agent/status", {
      Origin: "tauri://localhost",
    });
    assert.equal(status, 200);
    assert.equal(headers["access-control-allow-origin"], "tauri://localhost");
    assert.equal(headers["vary"], "Origin");
  } finally {
    await bus.stop();
  }
});

test("browser requests from untrusted Origins are rejected before route dispatch", async () => {
  let injected = false;
  const adapter = new MockAdapter({ agentId: "mock", tokensPerSecond: 60 });
  adapter.inject = async () => {
    injected = true;
    return { ok: true };
  };
  const { bus, port } = await startBus({ adapter });
  try {
    const health = await rawRequest(port, "GET", "/agent/health", {
      Origin: "https://evil.example",
    });
    assert.equal(health.status, 403);

    const inject = await rawRequest(port, "POST", "/agent/inject", {
      Origin: "https://evil.example",
      "Content-Type": "text/plain",
    }, JSON.stringify({ agentId: "mock", sessionId: "target", text: "attack" }));
    assert.equal(inject.status, 403);
    assert.equal(injected, false);
  } finally {
    await bus.stop();
  }
});

test("requests without Origin remain available to local sidecars", async () => {
  const { bus, port } = await startBus();
  try {
    const { status, body } = await jsonRequest(port, "GET", "/agent/health");
    assert.equal(status, 200);
    assert.equal(body.ok, true);
  } finally {
    await bus.stop();
  }
});

test("SSE inject response carries CORS headers", async () => {
  const { bus, port } = await startBus();
  try {
    const { status, headers } = await rawRequest(
      port,
      "POST",
      "/agent/inject",
      {
        Origin: "tauri://localhost",
        "Content-Type": "application/json",
        Accept: "text/event-stream",
      },
      { agentId: "mock", sessionId: "auto", text: "hi" },
    );
    assert.equal(status, 200);
    assert.equal(headers["access-control-allow-origin"], "tauri://localhost");
    assert.match(headers["content-type"] || "", /text\/event-stream/);
  } finally {
    await bus.stop();
  }
});
