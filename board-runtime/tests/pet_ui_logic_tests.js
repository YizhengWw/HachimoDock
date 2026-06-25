/*
 * [Input] board-runtime browser-side UI helpers and source files.
 * [Output] Regression coverage for task-card rendering limits plus shared board UI logic.
 */

const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { resolve } = require("node:path");

const vitals = require("../ui/windows/main/pet-vitals.js");
const detailStats = require("../ui/windows/main/detail-runtime-stats.js");

require("../ui/windows/main/websocket-bridge.js");

global.window = globalThis;
require("../ui/windows/main/detail-view.js");

function createMemoryStorage(initial = {}) {
  const values = new Map(Object.entries(initial));
  return {
    getItem(key) {
      return values.has(key) ? values.get(key) : null;
    },
    setItem(key, value) {
      values.set(key, String(value));
    },
    dump() {
      return Object.fromEntries(values);
    },
  };
}

function testPetVitalsDeltaTracking() {
  let state = vitals.applyPayloadToState(
    {},
    { source: "codex", sessionId: "session-a", tokenUsage: { totalTokens: 100 } },
    1_000,
  );
  assert.equal(state.lifetimeExp, 100);

  state = vitals.applyPayloadToState(
    state,
    { source: "codex", sessionId: "session-a", tokenUsage: { totalTokens: 150 } },
    2_000,
  );
  assert.equal(state.lifetimeExp, 150);

  state = vitals.applyPayloadToState(
    state,
    { source: "codex", sessionId: "session-a", tokenUsage: { totalTokens: 20 } },
    3_000,
  );
  assert.equal(state.lifetimeExp, 150);
  assert.equal(state.sessions["codex:session:session-a"].lastTotalTokens, 20);

  state = vitals.applyPayloadToState(
    state,
    { source: "codex", sessionId: "session-a", tokenUsage: { totalTokens: 50 } },
    4_000,
  );
  assert.equal(state.lifetimeExp, 180);
}

function testPetVitalsPersistenceAndDerivedMetrics() {
  const storage = createMemoryStorage({
    "test-key": JSON.stringify({
      lifetimeExp: 4000.9,
      sessions: {
        good: { lastTotalTokens: 12, updatedAt: 7 },
        bad: { lastTotalTokens: -1, updatedAt: 7 },
      },
    }),
  });
  const tracker = vitals.createPetVitalsTracker({ storage, storageKey: "test-key" });
  assert.equal(tracker.getLifetimeExp(), 4000);
  assert.deepEqual(Object.keys(tracker.getState().sessions), ["good"]);

  tracker.applyPayload(
    { source: "gemini", runId: "run-1", tokenUsage: { totalTokens: 500 } },
    10_000,
  );
  assert.equal(tracker.getLifetimeExp(), 4500);
  assert(JSON.parse(storage.dump()["test-key"]).sessions["gemini:run:run-1"]);

  const progress = vitals.computeLevelProgress(4000);
  assert.equal(progress.level, 3);
  assert.equal(progress.levelStartExp, 4000);
  assert.equal(progress.levelEndExp, 9000);
  assert.equal(progress.expPercent, 0);

  const computed = vitals.computePetVitals(
    {
      metrics: {
        contextUsagePct: 81,
        toolErrors: 2,
        latency: { firstTokenMs: 400 },
      },
      tokenUsage: {
        inputTokens: 100,
        cachedInputTokens: 40,
        lastInputTokens: 80,
        lastOutputTokens: 10,
      },
    },
    [{ totalTokens: 50 }, { totalTokens: -10 }],
    { lifetimeExp: 4000 },
  );
  assert.equal(computed.level, 3);
  assert.equal(computed.runtimeTokens, 50);
  assert.equal(computed.brainStatus, "\u6781\u5ea6\u75b2\u52b3");
  assert.equal(computed.cacheHit, "40.0");
  assert.equal(computed.moodFrustration, 40);
  assert.equal(computed.focusLevel, "\u6781\u5ea6\u5174\u594b");
  assert.equal(computed.isEating, true);
}

function testDetailRuntimeStatsGroupingAndMerging() {
  const stats = detailStats.buildDetailSourceStats([
    {
      updatedAt: 10,
      payload: {
        source: "codex",
        channel: "codex-log",
        tokenUsage: { totalTokens: 100 },
        metrics: { toolCalls: 2, toolErrors: 1 },
      },
    },
    {
      updatedAt: 30,
      payload: {
        source: "browser",
        channel: "active",
        metrics: { toolCalls: 1 },
      },
    },
    {
      updatedAt: 20,
      payload: {
        source: "codex",
        channel: "active",
        tokenUsage: { totalTokens: 50 },
        metrics: { toolCalls: 3 },
      },
    },
  ]);

  assert.equal(stats.length, 2);
  assert.equal(stats[0].source, "browser");
  const codex = stats.find((item) => item.source === "codex");
  assert.equal(codex.sourceLabel, "Codex");
  assert.deepEqual(codex.channels, ["codex-log", "active"]);
  assert.equal(codex.totalTokens, 150);
  assert.equal(codex.toolCalls, 5);
  assert.equal(codex.toolErrors, 1);

  const merged = detailStats.mergeDetailRuntimeSourceFrame(
    {
      topic: "desk/state/codex",
      activeTopic: false,
      updatedAt: 100,
      observedAt: 120,
      payload: {
        source: "codex",
        channel: "codex-log",
        tokenUsage: { totalTokens: 100 },
        metrics: { toolCalls: 2 },
      },
    },
    {
      topic: "desk/state/active",
      activeTopic: true,
      updatedAt: 90,
      observedAt: 130,
      bridgeAgeMs: 5,
      payload: {
        source: "codex",
        channel: "active",
        state: "working",
      },
    },
  );

  assert.equal(merged.topic, "desk/state/codex");
  assert.equal(merged.activeTopic, true);
  assert.equal(merged.payload.channel, "codex-log");
  assert.deepEqual(merged.payload.tokenUsage, { totalTokens: 100 });
  assert.deepEqual(merged.payload.metrics, { toolCalls: 2 });
  assert.equal(merged.updatedAt, 100);
  assert.equal(merged.observedAt, 130);
  assert.equal(merged.bridgeAgeMs, 5);

  assert.equal(detailStats.resolveDetailRuntimeSourceLabel("openclaw-gateway"), "OpenClaw");
  assert.equal(detailStats.resolveDetailRuntimeChannelLabel("active"), "\u4e3b\u72b6\u6001\u805a\u5408");
  assert.equal(
    detailStats.summarizeDetailRuntimeChannels(
      [{ sourceLabel: "Codex" }, { sourceLabel: "Claude Code" }, { sourceLabel: "Gemini" }],
      { limit: 2 },
    ),
    "Codex\u3001Claude Code \u7b49 3 \u4e2a\u6e20\u9053",
  );
  assert.deepEqual(
    detailStats.selectDetailRuntimeDisplayStats([{ source: "browser" }, { source: "codex" }]),
    [{ source: "codex" }],
  );
}

async function testWebSocketBridgeConfigConnectionAndMessages() {
  const previousWebSocket = global.WebSocket;
  const previousSetTimeout = global.setTimeout;
  const sockets = [];
  const timers = [];
  const messages = [];
  const errors = [];
  const infos = [];

  class FakeWebSocket {
    constructor(url) {
      this.url = url;
      sockets.push(this);
    }
  }

  global.WebSocket = FakeWebSocket;
  global.setTimeout = (fn, delay) => {
    timers.push({ fn, delay });
    return timers.length;
  };

  try {
    const bridge = global.createPetClawWebSocketBridge({
      desktopAPI: {
        async invoke(name) {
          assert.equal(name, "get-websocket-config");
          return { host: "localhost", port: "3010" };
        },
      },
      logger: {
        log() {},
        warn() {},
        error(...args) {
          errors.push(args);
        },
        info(...args) {
          infos.push(args);
        },
      },
      retryDelay: 123,
      onMessage(message) {
        messages.push(message);
      },
    });

    assert.deepEqual(await bridge.getWebSocketConfig(), { host: "localhost", port: 3010 });
    await bridge.connect();
    assert.equal(sockets.length, 1);
    assert.equal(sockets[0].url, "ws://localhost:3010");
    assert.equal(bridge.getSocket(), sockets[0]);

    const now = Date.now();
    sockets[0].onmessage({
      data: JSON.stringify({
        type: "bridge_state",
        _wsSeq: 2,
        _wsSentAt: now,
        _bridgeReceivedAt: now - 5,
        _bridgeAgeMs: 8,
        topic: "desk/state/codex",
        payload: {
          source: "codex",
          state: "working",
          event: "PreToolUse",
          tsMs: now - 20,
        },
      }),
    });
    assert.equal(messages.length, 1);
    assert.equal(messages[0].payload.source, "codex");
    assert.equal(infos.length, 1);

    sockets[0].onmessage({ data: "not-json" });
    assert.equal(errors.length, 1);

    sockets[0].onclose();
    assert.equal(timers.length, 1);
    assert.equal(timers[0].delay, 123);

    bridge.resetConfigCache();
    assert.deepEqual(await bridge.getWebSocketConfig(), { host: "localhost", port: 3010 });
  } finally {
    global.WebSocket = previousWebSocket;
    global.setTimeout = previousSetTimeout;
  }
}

function testDetailViewRenderer() {
  const root = { innerHTML: "" };
  global.renderPetClawDetailViewSection(root);
  assert(root.innerHTML.includes('id="detailAgentName"'));
  assert(root.innerHTML.includes('id="detailAppSpendList"'));
  assert(root.innerHTML.includes('aria-label="\u72b6\u6001\u9875"'));
}

function testTaskDashboardRendersOnlyOneTaskCard() {
  const source = readFileSync(resolve(__dirname, "../ui/windows/main/index.html"), "utf8");
  const match = source.match(/function renderTaskCards\(tasks\) \{[\s\S]*?\n\}/);
  assert(match, "expected renderTaskCards function");
  assert.match(
    match[0],
    /slice\(0,\s*1\)/,
    "expected task dashboard to render at most one task card",
  );
}

(async () => {
  testPetVitalsDeltaTracking();
  testPetVitalsPersistenceAndDerivedMetrics();
  testDetailRuntimeStatsGroupingAndMerging();
  await testWebSocketBridgeConfigConnectionAndMessages();
  testDetailViewRenderer();
  testTaskDashboardRendersOnlyOneTaskCard();
  console.log("pet ui logic tests passed");
})();
