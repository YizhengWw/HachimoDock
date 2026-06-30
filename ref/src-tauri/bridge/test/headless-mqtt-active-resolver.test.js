/*
 * [Input] Headless bridge HTTP/MQTT fixtures plus fake agent-session-bus streams.
 * [Output] Regression coverage for selected-source state, mock/board voice injection, and stale Codex metadata recovery.
 * [Pos] Integration-style Node tests for the managed status bridge.
 * [Sync] If voice-injection recovery behavior changes, update `ref/.folder.md`.
 */

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const http = require("node:http");
const os = require("node:os");
const path = require("node:path");

const {
  HookHttpServer,
  MqttPublisher,
  injectViaAgentBus,
  mapClawdStateToStatus,
  normalizeStatus,
  resolveMockButtonInjectRequest,
} = require("../packages/clawd-backend-service/src/headless-mqtt");
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

test("MqttPublisher publishes per-source topic without active aggregation", () => {
  const published = [];
  const localStateDir = fs.mkdtempSync(path.join(os.tmpdir(), "pet-manager-state-test-"));
  const publisher = new MqttPublisher({
    url: "mqtt://example.invalid",
    clientId: "test",
    qos: 0,
    retain: true,
    reconnectMs: 1000,
    namespace: "desk",
    deviceId: "devbox",
    activeTopicSuffix: "active",
    enableActiveTopic: false,
    localStateDir,
  });
  publisher.connected = true;
  publisher.client = {
    publish(topic, payloadText, options) {
      published.push({ topic, payload: JSON.parse(payloadText), options });
    },
  };

  publisher.publishSource({
    source: "codex",
    state: "done",
    sessionId: "session-a",
    event: "AssistantMessage",
    tsMs: 10000,
  });

  assert.equal(published.length, 1);
  assert.equal(published[0].topic, "desk/devbox/state/codex");
  assert.equal(published[0].payload.state, "done");
  assert.equal(published[0].payload.sessionId, "session-a");
  assert.equal(published[0].options.retain, true);
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

test("MqttPublisher clears retained state for disabled sources", () => {
  const published = [];
  const publisher = new MqttPublisher({
    url: "mqtt://example.invalid",
    clientId: "test",
    qos: 1,
    retain: true,
    reconnectMs: 1000,
    namespace: "desk",
    deviceId: "devbox",
    activeTopicSuffix: "active",
    enableActiveTopic: false,
    enabledSources: ["claude-code"],
  });
  publisher.connected = true;
  publisher.client = {
    publish(topic, payloadText, options) {
      published.push({ topic, payloadText, options });
    },
  };

  publisher.clearDisabledSourceTopics();

  assert.deepEqual(
    published.map((item) => item.topic).sort(),
    ["desk/devbox/state/codex", "desk/devbox/state/openclaw"],
  );
  assert.equal(published[0].payloadText, "");
  assert.equal(published[0].options.retain, true);
});

test("MqttPublisher reports first enabled source as preferred source", () => {
  const publisher = new MqttPublisher({
    url: "mqtt://example.invalid",
    clientId: "test",
    qos: 1,
    retain: true,
    reconnectMs: 1000,
    namespace: "desk",
    deviceId: "devbox",
    activeTopicSuffix: "active",
    enableActiveTopic: false,
    enabledSources: ["claude-code", "codex"],
  });
  assert.equal(publisher.getPreferredSource(), "claude-code");

  const emptyPublisher = new MqttPublisher({
    url: "mqtt://example.invalid",
    clientId: "test-empty",
    qos: 1,
    retain: true,
    reconnectMs: 1000,
    namespace: "desk",
    deviceId: "devbox",
    activeTopicSuffix: "active",
    enableActiveTopic: false,
    enabledSources: [],
  });
  assert.equal(emptyPublisher.getPreferredSource(), "");
});

test("MqttPublisher uses selected source for shared speech channel", () => {
  const published = [];
  const localStateDir = fs.mkdtempSync(path.join(os.tmpdir(), "pet-manager-speech-test-"));
  const publisher = new MqttPublisher({
    url: "mqtt://example.invalid",
    clientId: "test-speech",
    qos: 1,
    retain: true,
    reconnectMs: 1000,
    namespace: "desk",
    deviceId: "devbox",
    activeTopicSuffix: "active",
    enableActiveTopic: false,
    enabledSources: ["codex", "claude-code"],
    selectedSource: "claude-code",
    localStateDir,
  });
  publisher.connected = true;
  publisher.client = {
    publish(topic, payloadText, options) {
      published.push({ topic, payload: JSON.parse(payloadText), options });
    },
  };

  assert.equal(publisher.getPreferredSource(), "claude-code");
  publisher.publishSpeech("codex", { displayContent: "codex done" });
  publisher.publishSpeech("claude-code", { displayContent: "claude done" });

  assert.equal(published.length, 1);
  assert.equal(published[0].topic, "desk/devbox/speech/text");
  assert.equal(published[0].payload.source, "claude-code");
  assert.equal(published[0].payload.displayContent, "claude done");
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

test("MqttPublisher resolves wireless remote binding to an online board for the same desktop", () => {
  const publisher = new MqttPublisher({
    url: "mqtt://example.invalid",
    clientId: "test",
    qos: 1,
    retain: true,
    reconnectMs: 1000,
    namespace: "desk",
    deviceId: "devbox",
    activeTopicSuffix: "active",
    enableActiveTopic: false,
  });

  publisher._handleMessage(
    "claw-pet/board/board-new/availability",
    Buffer.from(JSON.stringify({
      online: true,
      boardDeviceId: "board-new",
      localDeviceId: "board-local-new",
      targetDeviceId: "devbox",
      targetSource: "codex",
      mqttNamespace: "desk",
      ts: "2026-05-22T08:00:00.000Z",
    })),
  );
  publisher._handleMessage(
    "claw-pet/board/board-other/availability",
    Buffer.from(JSON.stringify({
      online: true,
      boardDeviceId: "board-other",
      targetDeviceId: "someone-else",
      mqttNamespace: "desk",
    })),
  );

  assert.deepEqual(
    publisher.resolveRemoteBindingBoardIds({
      boardDeviceId: "board-stale",
      targetDeviceId: "devbox",
      mqttNamespace: "desk",
    }),
    ["board-stale", "board-new"],
  );
  assert.deepEqual(
    publisher.resolveRemoteBindingTargets({
      boardDeviceId: "board-stale",
      targetDeviceId: "devbox",
      mqttNamespace: "desk",
    }).map((target) => target.controlTopic),
    [
      "desk/board-stale/control/remote-cli-binding",
      "desk/board-local-new/control/remote-cli-binding",
    ],
  );
  assert.equal(publisher.getDeviceAvailability()["board-new"].targetDeviceId, "devbox");
});

test("MqttPublisher can fall back to the only online board when a dev binding is stale", () => {
  const publisher = new MqttPublisher({
    url: "mqtt://example.invalid",
    clientId: "test",
    qos: 1,
    retain: true,
    reconnectMs: 1000,
    namespace: "desk",
    deviceId: "devbox",
    activeTopicSuffix: "active",
    enableActiveTopic: false,
  });

  publisher._handleMessage(
    "claw-pet/board/board-online/availability",
    Buffer.from(JSON.stringify({
      online: true,
      boardDeviceId: "board-online",
      targetDeviceId: "formal-desktop",
      mqttNamespace: "desk",
    })),
  );

  assert.deepEqual(
    publisher.resolveRemoteBindingBoardIds({
      boardDeviceId: "board-stale",
      targetDeviceId: "devbox",
      mqttNamespace: "desk",
    }),
    ["board-stale", "board-online"],
  );
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
