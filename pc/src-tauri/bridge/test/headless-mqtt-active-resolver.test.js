/*
 * [Input] Headless bridge HTTP/MQTT fixtures plus fake agent-session-bus streams.
 * [Output] Regression coverage for selected-source state, token retention/context metrics, removal of MQTT follow publishing, Claude Desktop hook display metadata, mock/board voice injection, and stale Codex metadata recovery.
 * [Pos] Integration-style Node tests for the managed status bridge.
 * [Sync] If voice-injection recovery behavior changes, update `pc/.folder.md`.
 */

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const http = require("node:http");
const os = require("node:os");
const path = require("node:path");
const headlessSource = fs.readFileSync(
  path.join(__dirname, "../packages/clawd-backend-service/src/headless-mqtt.js"),
  "utf8",
);

const {
  HookHttpServer,
  LocalStatePublisher,
  injectViaAgentBus,
  mapClawdStateToStatus,
  normalizeStatus,
  resolveMockButtonInjectRequest,
  isAgentBusyState,
  createLatestHardwareInputQueue,
  buildClaudeHookDisplay,
  buildMiMoCodeHookDisplay,
} = require("../packages/clawd-backend-service/src/headless-mqtt");
const {
  SessionMetricsTracker,
  calculateContextUsagePct,
} = require("../packages/clawd-backend-service/src/status-metrics");
const { createAgentSessionBus, MockAdapter } = require("../packages/agent-session-bus/src/index");

function postJson(port, pathname, body) {
  return new Promise((resolve, reject) => {
    const req = http.request({
      host: "127.0.0.1",
      port,
      method: "POST",
      path: pathname,
      headers: {
        "Content-Type": "application/json",
      },
    }, (res) => {
      const chunks = [];
      res.on("data", (chunk) => chunks.push(chunk));
      res.on("end", () => {
        const raw = Buffer.concat(chunks).toString("utf8");
        let parsed = null;
        try {
          parsed = JSON.parse(raw);
        } catch {
          parsed = { raw };
        }
        resolve({
          statusCode: res.statusCode,
          body: parsed,
        });
      });
    });
    req.on("error", reject);
    req.write(JSON.stringify(body || {}));
    req.end();
  });
}

test("headless bridge contains no legacy MQTT device transport", () => {
  assert.doesNotMatch(headlessSource, /publish-remote-binding/);
  assert.doesNotMatch(headlessSource, /resolveRemoteBindingTargets/);
  assert.doesNotMatch(headlessSource, /mqtt\.connect|claw-pet\/board|publish-command|publish-test|device-availability/);
});

test("Claude hook events produce stable device card content", () => {
  const working = buildClaudeHookDisplay({ prompt: "修复 Claude 客户端的前台语音输入" }, "working", "UserPromptSubmit");
  assert.equal(working.title, "修复 Claude 客户端的前台语音输入");
  assert.equal(working.content, "正在思考");

  const done = buildClaudeHookDisplay({
    last_assistant_message: "已完成会话定位和输入框提交。",
  }, "done", "Stop");
  assert.equal(done.content, "已完成会话定位和输入框提交。");
});

test("MiMoCode plugin events use the shared device card shape", () => {
  const working = buildMiMoCodeHookDisplay({
    display_title: "适配 MiMoCode",
    display_content: "正在执行 bash",
  }, "working", "tool.running");
  assert.equal(working.title, "适配 MiMoCode");
  assert.equal(working.content, "正在执行 bash");

  const done = buildMiMoCodeHookDisplay({
    session_title: "适配 MiMoCode",
    last_assistant_message: "状态同步已经完成。",
  }, "done", "session.post");
  assert.equal(done.title, "适配 MiMoCode");
  assert.equal(done.content, "状态同步已经完成。");
});

test("Claude hook-only sessions keep their first inferred navigation title", () => {
  const tracker = new SessionMetricsTracker();
  const now = Date.now();
  tracker.apply({
    source: "claude-code",
    sessionId: "desktop-session",
    sessionTitle: "第一条任务",
    display: { title: "第一条任务", content: "正在思考" },
    state: "working",
  }, now);
  tracker.apply({
    source: "claude-code",
    sessionId: "desktop-session",
    sessionTitle: "后续追问",
    display: { title: "后续追问", content: "正在思考" },
    state: "working",
  }, now + 1);

  const [session] = tracker.sessionStatuses("claude-code");
  assert.equal(session.title, "第一条任务");
  assert.equal(session.displayTitle, "后续追问");
});

test("Claude terminal state ignores delayed trailing lifecycle events until a new prompt", () => {
  const tracker = new SessionMetricsTracker();
  const now = Date.now();
  const base = {
    source: "claude-code",
    sessionId: "desktop-session",
    sessionTitle: "状态回归测试",
  };
  tracker.apply({
    ...base,
    event: "UserPromptSubmit",
    state: "working",
    display: { title: "状态回归测试", content: "正在思考" },
  }, now);
  tracker.apply({
    ...base,
    event: "claude:assistant_message",
    state: "done",
    display: { title: "状态回归测试", content: "任务已经完成。" },
  }, now + 10);

  const delayedWorking = tracker.apply({
    ...base,
    event: "SubagentStop",
    state: "working",
    display: { title: "状态回归测试", content: "正在处理" },
  }, now + 20);
  const delayedIdle = tracker.apply({
    ...base,
    event: "SessionEnd",
    state: "idle",
  }, now + 30);

  assert.equal(delayedWorking.state, "done");
  assert.equal(delayedWorking.display.content, "任务已经完成。");
  assert.equal(delayedIdle.state, "done");
  const [completed] = tracker.sessionStatuses("claude-code", now + 30);
  assert.equal(completed.state, "done");
  assert.equal(completed.displayContent, "任务已经完成。");
  assert.equal(completed.updatedAt, now + 10);

  const nextTurn = tracker.apply({
    ...base,
    event: "UserPromptSubmit",
    state: "working",
    display: { title: "新的任务", content: "正在思考" },
  }, now + 40);
  assert.equal(nextTurn.state, "working");
  const [working] = tracker.sessionStatuses("claude-code", now + 40);
  assert.equal(working.state, "working");
  assert.equal(working.displayContent, "正在思考");
});

test("SessionMetricsTracker retains token usage across usage-free lifecycle events", () => {
  const tracker = new SessionMetricsTracker();
  const first = tracker.apply({
    source: "claude-code",
    sessionId: "session-with-usage",
    state: "speaking",
    tokenUsage: {
      totalTokens: 120,
      inputTokens: 90,
      outputTokens: 30,
      lastInputTokens: 45,
      modelContextWindow: 200,
    },
  }, 1000);
  const terminal = tracker.apply({
    source: "claude-code",
    sessionId: "session-with-usage",
    state: "done",
    event: "SessionEnd",
  }, 1100);
  const differentSession = tracker.apply({
    source: "claude-code",
    sessionId: "different-session",
    state: "working",
  }, 1200);

  assert.deepEqual(terminal.tokenUsage, first.tokenUsage);
  assert.equal(terminal.metrics.contextUsagePct, 22.5);
  assert.equal(differentSession.tokenUsage, undefined);
});

test("context usage prefers latest input over cumulative session totals", () => {
  assert.equal(calculateContextUsagePct({
    totalTokens: 637353,
    inputTokens: 630814,
    lastInputTokens: 93130,
    modelContextWindow: 258400,
  }), 36.04);
  assert.equal(calculateContextUsagePct({
    totalTokens: 300,
    modelContextWindow: 200,
  }), 100);
});

test("LocalStatePublisher writes per-source snapshots for USB polling", () => {
  const localStateDir = fs.mkdtempSync(path.join(os.tmpdir(), "pet-manager-state-test-"));
  const publisher = new LocalStatePublisher({
    localStateDir,
  });
  publisher.publishSource({
    source: "codex",
    state: "done",
    sessionId: "session-a",
    event: "AssistantMessage",
    tsMs: 10000,
  });

  assert.equal(publisher.getSourceState("codex"), "done");
  assert.equal(
    fs.readFileSync(path.join(localStateDir, "codex.json"), "utf8").trim(),
    JSON.stringify({
      source: "codex",
      state: "done",
      sessionId: "session-a",
      event: "AssistantMessage",
      tsMs: 10000,
    }),
  );
  const sessionFiles = fs.readdirSync(localStateDir).filter((name) => name.startsWith("codex--session-"));
  assert.equal(sessionFiles.length, 1);
  assert.equal(
    fs.readFileSync(path.join(localStateDir, sessionFiles[0]), "utf8").trim(),
    fs.readFileSync(path.join(localStateDir, "codex.json"), "utf8").trim(),
  );
});

test("Codex monitor config can target an explicit session directory", () => {
  const modulePath = require.resolve("../agents/codex");
  const previousSessionDir = process.env.CLAWD_CODEX_SESSION_DIR;
  delete require.cache[modulePath];
  process.env.CLAWD_CODEX_SESSION_DIR = "/tmp/pet-manager-real-codex/sessions";

  try {
    const config = require(modulePath);
    assert.equal(config.SESSION_DIR, "/tmp/pet-manager-real-codex/sessions");
  } finally {
    if (typeof previousSessionDir === "string") {
      process.env.CLAWD_CODEX_SESSION_DIR = previousSessionDir;
    } else {
      delete process.env.CLAWD_CODEX_SESSION_DIR;
    }
    delete require.cache[modulePath];
  }
});

test("LocalStatePublisher reports first enabled source as preferred source", () => {
  const publisher = new LocalStatePublisher({
    enabledSources: ["claude-code", "codex"],
  });
  assert.equal(publisher.getPreferredSource(), "claude-code");

  const emptyPublisher = new LocalStatePublisher({
    enabledSources: [],
  });
  assert.equal(emptyPublisher.getPreferredSource(), "");
});

test("LocalStatePublisher writes speech only for the selected source", () => {
  const localStateDir = fs.mkdtempSync(path.join(os.tmpdir(), "pet-manager-speech-test-"));
  const publisher = new LocalStatePublisher({
    enabledSources: ["codex", "claude-code"],
    selectedSource: "claude-code",
    localStateDir,
  });
  publisher._localSpeechDir = localStateDir;

  assert.equal(publisher.getPreferredSource(), "claude-code");
  publisher.publishSpeech("codex", { displayContent: "codex done" });
  publisher.publishSpeech("claude-code", { displayContent: "claude done" });

  const speech = JSON.parse(fs.readFileSync(path.join(localStateDir, "claude-code.json"), "utf8"));
  assert.equal(speech.source, "claude-code");
  assert.equal(speech.displayContent, "claude done");
  assert.equal(fs.existsSync(path.join(localStateDir, "codex.json")), false);
});

test("resolveMockButtonInjectRequest applies defaults for mock button path", () => {
  const resolved = resolveMockButtonInjectRequest({}, {
    defaultAgentId: "codex",
    defaultSessionId: "auto",
    defaultText: "固定测试文本",
  });

  assert.equal(resolved.injectBody.agentId, "codex");
  assert.equal(resolved.injectBody.sessionId, "auto");
  assert.equal(resolved.injectBody.text, "固定测试文本");
  assert.equal(resolved.injectBody.metadata.source, "mock-button");
  assert.equal(resolved.injectBody.metadata.inputType, "mock-text");
  assert.equal(resolved.injectBody.metadata.trigger, "device-button");
  assert.equal(resolved.injectBody.metadata.buttonEvent, "button.primary.short_press");
  assert.equal(typeof resolved.injectBody.metadata.ts, "string");
});

test("resolveMockButtonInjectRequest honors explicit payload overrides", () => {
  const resolved = resolveMockButtonInjectRequest({
    agentId: "claude-code",
    sessionId: "my-session",
    text: "自定义文本",
    buttonEvent: "button.primary.long_press",
    metadata: {
      source: "device-stt",
      locale: "zh-CN",
    },
  }, {
    defaultAgentId: "codex",
    defaultSessionId: "auto",
    defaultText: "默认文本",
  });

  assert.equal(resolved.injectBody.agentId, "claude-code");
  assert.equal(resolved.injectBody.sessionId, "my-session");
  assert.equal(resolved.injectBody.text, "自定义文本");
  assert.equal(resolved.injectBody.metadata.source, "device-stt");
  assert.equal(resolved.injectBody.metadata.locale, "zh-CN");
  assert.equal(resolved.injectBody.metadata.buttonEvent, "button.primary.long_press");
});

test("headless bridge normalizes legacy thinking/tool states to unified working", () => {
  assert.equal(normalizeStatus("working"), "working");
  assert.equal(normalizeStatus("thinking"), "working");
  assert.equal(normalizeStatus("tool_running"), "working");
  assert.equal(mapClawdStateToStatus("thinking", "UserPromptSubmit"), "working");
  assert.equal(mapClawdStateToStatus("working", "PreToolUse"), "working");
  assert.equal(mapClawdStateToStatus("juggling", "SubagentStart"), "working");
});

test("headless bridge keeps user decision requests out of generic working", () => {
  assert.equal(mapClawdStateToStatus("notification", "Elicitation"), "waiting_user");
  assert.equal(mapClawdStateToStatus("codex-permission", "PermissionRequest"), "waiting_user");
});

test("HookHttpServer /mock-button-inject delegates request to callback", async () => {
  const server = new HookHttpServer({
    port: 0,
    onState() {},
    onPermission() { return { behavior: "allow" }; },
    onMockButtonInject: async (payload) => ({
      echoedText: payload.text,
      echoedAgentId: payload.agentId,
    }),
  });
  const port = await server.start();
  try {
    const response = await postJson(port, "/mock-button-inject", {
      text: "hello from test",
      agentId: "codex",
    });
    assert.equal(response.statusCode, 200);
    assert.equal(response.body.ok, true);
    assert.equal(response.body.action, "mock-button-inject");
    assert.equal(response.body.echoedText, "hello from test");
    assert.equal(response.body.echoedAgentId, "codex");
  } finally {
    server.stop();
  }
});

test("hardware input busy states identify Agent turns that need queueing", () => {
  assert.equal(isAgentBusyState("working"), true);
  assert.equal(isAgentBusyState("speaking"), true);
  assert.equal(isAgentBusyState("waiting_user"), true);
  assert.equal(isAgentBusyState("idle"), false);
  assert.equal(isAgentBusyState("done"), false);
  assert.equal(isAgentBusyState("error"), false);
});

test("busy hardware voice keeps only the latest input and sends it after the turn", async (t) => {
  let busy = true;
  const injected = [];
  const queue = createLatestHardwareInputQueue({
    retryMs: 10,
    ttlMs: 1000,
    isBusy: () => busy,
    inject: async (value) => injected.push(value.text),
  });
  t.after(() => queue.stop());

  assert.deepEqual(queue.enqueue("codex", { text: "first" }), {
    queued: true,
    replaced: false,
  });
  assert.deepEqual(queue.enqueue("codex", { text: "latest" }), {
    queued: true,
    replaced: true,
  });
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.deepEqual(injected, []);

  busy = false;
  for (let attempt = 0; attempt < 20 && injected.length === 0; attempt += 1) {
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  assert.deepEqual(injected, ["latest"]);
});

test("hardware voice queues when Agent Bus becomes busy during injection", async (t) => {
  let reportedBusy = false;
  let attempts = 0;
  const injected = [];
  const queue = createLatestHardwareInputQueue({
    retryMs: 10,
    ttlMs: 1000,
    isBusy: () => reportedBusy,
    inject: async (value) => {
      attempts += 1;
      if (attempts === 1) {
        reportedBusy = true;
        const error = new Error("agent codex is busy");
        error.code = "AGENT_BUSY";
        throw error;
      }
      injected.push(value.text);
      return { done: true };
    },
  });
  t.after(() => queue.stop());

  assert.deepEqual(await queue.submit("codex", { text: "voice after race" }), {
    queued: true,
    replaced: false,
  });
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.equal(attempts, 1);

  reportedBusy = false;
  for (let attempt = 0; attempt < 20 && injected.length === 0; attempt += 1) {
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  assert.equal(attempts, 2);
  assert.deepEqual(injected, ["voice after race"]);
});

test("HookHttpServer strict port mode rejects conflicts instead of drifting", async (t) => {
  let blocker = null;
  let blockedPort = null;
  for (let candidate = 23333; candidate <= 23337; candidate += 1) {
    const attempt = http.createServer();
    try {
      await new Promise((resolve, reject) => {
        attempt.once("error", reject);
        attempt.listen(candidate, "127.0.0.1", resolve);
      });
      blocker = attempt;
      blockedPort = candidate;
      break;
    } catch {
      try { attempt.close(); } catch {}
    }
  }
  if (!blocker) {
    t.skip("all managed bridge ports are already occupied");
    return;
  }

  const server = new HookHttpServer({
    port: blockedPort,
    strictPort: true,
    onState() {},
    onPermission() { return { behavior: "allow" }; },
  });
  try {
    await assert.rejects(
      server.start(),
      new RegExp(`unable to bind http server.*${blockedPort}`),
    );
    assert.equal(server.port, null);
  } finally {
    server.stop();
    await new Promise((resolve) => blocker.close(resolve));
  }
});

test("injectViaAgentBus can send mock button text into a live bus session", async () => {
  const bus = createAgentSessionBus({
    port: 0,
    adapters: [new MockAdapter({ agentId: "mock", tokensPerSecond: 240 })],
    log: () => {},
  });
  await bus.start();
  try {
    const resolved = resolveMockButtonInjectRequest({
      agentId: "mock",
      text: "设备按钮模拟内容",
    }, {
      defaultSessionId: "auto",
      defaultText: "fallback text",
    });

    const result = await injectViaAgentBus(bus, resolved.injectBody, { timeoutMs: 10000 });
    assert.equal(typeof result.ready.runId, "string");
    assert.equal(typeof result.ready.sessionId, "string");
    assert.equal(typeof result.done.sessionId, "string");
    assert.ok(result.tokenChars > 0);
  } finally {
    await bus.stop();
  }
});

test("injectViaAgentBus retries Codex metadata session failures with a fresh voice session", async () => {
  const requests = [];
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => {
      const body = JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}");
      requests.push(body);
      res.writeHead(200, { "Content-Type": "text/event-stream" });
      if (requests.length === 1) {
        res.write("event: error\n");
        res.write("data: {\"code\":\"-32603\",\"message\":\"failed to read thread: rollout does not start with session metadata\"}\n\n");
        res.end();
        return;
      }
      res.write("event: ready\n");
      res.write("data: {\"runId\":\"run-fresh\",\"agentId\":\"codex\",\"sessionId\":\"fresh-session\",\"opened\":true}\n\n");
      res.write("event: token\n");
      res.write("data: {\"text\":\"ok\"}\n\n");
      res.write("event: done\n");
      res.write("data: {\"sessionId\":\"fresh-session\",\"stopReason\":\"end_turn\"}\n\n");
      res.end();
    });
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const port = server.address().port;
  const fakeBus = { port_: () => port };
  try {
    const result = await injectViaAgentBus(fakeBus, {
      agentId: "codex",
      sessionId: "auto",
      text: "voice input",
      metadata: { source: "board-voice-ptt" },
    }, { timeoutMs: 10000 });
    assert.equal(result.done.sessionId, "fresh-session");
    assert.deepEqual(requests.map((request) => request.sessionId), ["auto", "new"]);
    assert.equal(result.recoveredSession, true);
    assert.equal(requests[1].metadata.source, "board-voice-ptt");
    assert.equal(requests[1].metadata.recoveredFromSessionMetadataError, true);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

test("SessionMetricsTracker preserves ordered working and terminal events", () => {
  const tracker = new SessionMetricsTracker({ sessionEventLimit: 8 });
  const baseline = tracker.sessionEvents("codex");
  assert.equal(baseline.cursor, 0);
  assert.deepEqual(baseline.events, []);

  tracker.apply({
    source: "codex",
    sessionId: "codex:fast-session",
    event: "event_msg:task_started",
    state: "working",
    sessionTitle: "Fast task",
    display: { title: "Fast task", content: "正在处理" },
  }, 10_000);
  tracker.apply({
    source: "codex",
    sessionId: "codex:fast-session",
    event: "event_msg:task_complete",
    state: "done",
    sessionTitle: "Fast task",
    display: { title: "Fast task", content: "已完成" },
  }, 10_050);

  const update = tracker.sessionEvents("codex", {
    cursor: baseline.cursor,
    streamId: baseline.streamId,
  });
  assert.deepEqual(update.events.map((event) => event.state), ["working", "done"]);
  assert.deepEqual(
    update.events.map((event) => event.session.statusUpdatedAt),
    [10_000, 10_050],
  );

  const coldBootstrap = tracker.sessionEvents("codex");
  assert.deepEqual(coldBootstrap.events, []);
});
