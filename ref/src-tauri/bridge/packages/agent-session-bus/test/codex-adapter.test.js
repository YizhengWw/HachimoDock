"use strict";

const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { CodexAdapter } = require("../src/adapters/codex");

const FAKE_CODEX = path.join(__dirname, "fixtures", "fake-codex.js");

function fakeCodexEntryForHost(home) {
  if (process.platform !== "win32") return FAKE_CODEX;
  const shim = path.join(home, "fake-codex.cmd");
  const nodePath = process.execPath.replace(/"/g, '""');
  const scriptPath = FAKE_CODEX.replace(/"/g, '""');
  fs.writeFileSync(shim, `@echo off\r\n"${nodePath}" "${scriptPath}" %*\r\n`, "utf8");
  return shim;
}

async function withFakeHome(setup, fn) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "bus-codex-test-"));
  if (setup && setup.sessions) {
    for (const [folder, sessions] of Object.entries(setup.sessions)) {
      const dir = path.join(home, ".codex", "sessions", folder);
      fs.mkdirSync(dir, { recursive: true });
      for (const [sid, entry] of Object.entries(sessions)) {
        const spec = entry && typeof entry === "object"
          ? entry
          : { mtimeAgo: entry };
        const fileName = sid.endsWith(".jsonl") ? sid : `${sid}.jsonl`;
        const file = path.join(dir, fileName);
        const lines = [];
        if (spec.meta) lines.push({ type: "session_meta", payload: spec.meta });
        if (Array.isArray(spec.lines)) lines.push(...spec.lines);
        const payload = lines.length
          ? `${lines.map((line) => JSON.stringify(line)).join("\n")}\n`
          : "";
        fs.writeFileSync(file, payload);
        const mtimeAgo = Number.isFinite(spec.mtimeAgo) ? spec.mtimeAgo : 0;
        const mtime = (Date.now() - mtimeAgo) / 1000;
        fs.utimesSync(file, mtime, mtime);
      }
    }
  }
  if (setup && Array.isArray(setup.sessionIndex)) {
    const indexPath = path.join(home, ".codex", "session_index.jsonl");
    fs.mkdirSync(path.dirname(indexPath), { recursive: true });
    fs.writeFileSync(
      indexPath,
      setup.sessionIndex.map((entry) => JSON.stringify(entry)).join("\n") + "\n",
      "utf8",
    );
  }
  if (setup && setup.modelsCache) {
    const modelsPath = path.join(home, ".codex", "models_cache.json");
    fs.mkdirSync(path.dirname(modelsPath), { recursive: true });
    fs.writeFileSync(modelsPath, JSON.stringify(setup.modelsCache), "utf8");
  }
  try {
    return await fn(home);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
}

function makeAdapter(home, extra = {}) {
  const { env: extraEnv = {}, ...rest } = extra;
  return new CodexAdapter({
    env: {
      HOME: home,
      PATH: process.env.PATH,
      CODEX_CLI_PATH: fakeCodexEntryForHost(home),
      ...extraEnv,
    },
    cwd: home,
    fallbackPaths: [],
    extraPathDirs: [],
    ...rest,
  });
}

test("isAvailable: false when CODEX_CLI_PATH points to nothing", async () => {
  await withFakeHome(null, async (home) => {
    const a = new CodexAdapter({
      env: { HOME: home, PATH: "", CODEX_CLI_PATH: "/no/such/file" },
      cwd: home,
      fallbackPaths: [],
      extraPathDirs: [],
    });
    const probe = await a.isAvailable();
    assert.equal(probe.ready, false);
    assert.match(probe.reason, /未找到/);
  });
});

test("isAvailable: true when CODEX_CLI_PATH points to fake-codex", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home);
    const probe = await a.isAvailable();
    assert.equal(probe.ready, true, probe.reason || "should be ready");
  });
});

test("isAvailable: true when codex --version writes version to stderr", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home, {
      env: {
        FAKE_CODEX_VERSION_STDERR: "1",
      },
    });
    const probe = await a.isAvailable();
    assert.equal(probe.ready, true, probe.reason || "should be ready");
  });
});

test("isAvailable: false when version is below minVersion", async () => {
  await withFakeHome(null, async (home) => {
    const a = new CodexAdapter({
      env: {
        HOME: home,
        PATH: process.env.PATH,
        CODEX_CLI_PATH: fakeCodexEntryForHost(home),
        FAKE_CODEX_VERSION: "0.30.0",
      },
      cwd: home,
      minVersion: "0.40.0",
      fallbackPaths: [],
      extraPathDirs: [],
    });
    const probe = await a.isAvailable();
    assert.equal(probe.ready, false);
    assert.match(probe.reason, /低于/);
  });
});

test("default resolver paths include Windows app aliases and npm globals", async () => {
  await withFakeHome(null, async (home) => {
    const appData = path.join(home, "AppData", "Roaming");
    const localAppData = path.join(home, "AppData", "Local");
    const a = new CodexAdapter({
      env: {
        USERPROFILE: home,
        APPDATA: appData,
        LOCALAPPDATA: localAppData,
        PATH: "",
      },
      cwd: home,
    });
    assert.ok(a._defaultFallbackPaths().includes(`${localAppData}\\Microsoft\\WindowsApps\\codex.exe`));
    assert.ok(a._defaultFallbackPaths().includes(`${localAppData}\\OpenAI\\Codex\\bin\\codex.exe`));
    assert.ok(a._defaultExtraPathDirs().includes(`${appData}\\npm`));
    assert.ok(a._defaultExtraPathDirs().includes(`${localAppData}\\OpenAI\\Codex\\bin`));
  });
});

test("listSessions returns [] when ~/.codex/sessions does not exist", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    assert.deepEqual(sessions, []);
  });
});

test("listSessions walks dated folders and sorts newest-first", async () => {
  await withFakeHome({
    sessions: {
      "2026-04-27": { "old-sid": 60_000 },
      "2026-04-28": { "newest-sid": 1_000, "mid-sid": 30_000 },
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    assert.equal(sessions.length, 3);
    assert.equal(sessions[0].id, "newest-sid");
    assert.equal(sessions[2].id, "old-sid");
  });
});

test("listSessions skips Codex bootstrap context when building summaries", async () => {
  await withFakeHome({
    sessions: {
      "2026/06/03": {
        "rollout-2026-06-03T19-32-42-019e8d41-d656-7cf2-86ea-e7140b73a63e.jsonl": {
          mtimeAgo: 1_000,
          meta: { id: "voice-session", cwd: "/repo" },
          lines: [
            {
              type: "response_item",
              payload: {
                type: "message",
                role: "user",
                content: [{ text: "# AGENTS.md instructions for /repo\n\n<INSTRUCTIONS>" }],
              },
            },
            {
              type: "response_item",
              payload: {
                type: "message",
                role: "user",
                content: [{ text: "<environment_context>\n  <cwd>/repo</cwd>\n</environment_context>" }],
              },
            },
            {
              type: "response_item",
              payload: {
                type: "message",
                role: "user",
                content: [{ text: "请用一句中文回复：语音链路测试" }],
              },
            },
          ],
        },
      },
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    assert.equal(sessions[0].summary, "请用一句中文回复：语音链路测试");
  });
});

test("listSessions uses Codex client thread names and prioritizes named threads", async () => {
  await withFakeHome({
    sessionIndex: [
      {
        id: "client-thread",
        thread_name: "打招呼",
        updated_at: "2026-06-03T09:00:00.0000000Z",
      },
      {
        id: "older-active-thread",
        thread_name: "旧的客户端会话",
        updated_at: "2026-06-03T12:00:00.0000000Z",
      },
    ],
    sessions: {
      "2026/06/03": {
        "rollout-2026-06-03T19-27-05-client-thread.jsonl": {
          mtimeAgo: 1_000,
          meta: { id: "client-thread", cwd: "/repo" },
        },
        "rollout-2026-06-03T19-20-00-older-active-thread.jsonl": {
          mtimeAgo: 120_000,
          meta: { id: "older-active-thread", cwd: "/repo" },
        },
        "rollout-2026-06-03T19-32-42-exec-only.jsonl": {
          mtimeAgo: 100,
          meta: { id: "exec-only", cwd: "/repo" },
        },
      },
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    assert.equal(sessions[0].id, "client-thread");
    assert.equal(sessions[0].name, "打招呼");
    assert.equal(sessions[1].id, "older-active-thread");
    assert.equal(sessions[2].id, "exec-only");
    const active = await a.resolveActive();
    assert.equal(active.id, "client-thread");
  });
});

test("listSessions annotates recent Codex model support", async () => {
  await withFakeHome({
    modelsCache: {
      models: [
        { slug: "gpt-5.5" },
      ],
    },
    sessions: {
      "2026/06/03": {
        "rollout-2026-06-03T19-32-42-supported-thread.jsonl": {
          mtimeAgo: 1_000,
          meta: { id: "supported-thread", cwd: "/repo" },
          lines: [
            { type: "turn_context", payload: { model: "gpt-5.3-codex" } },
            { type: "turn_context", payload: { model: "gpt-5.5" } },
          ],
        },
        "rollout-2026-06-03T19-31-42-unsupported-thread.jsonl": {
          mtimeAgo: 2_000,
          meta: { id: "unsupported-thread", cwd: "/repo" },
          lines: [
            { type: "turn_context", payload: { model: "gpt-5.3-codex" } },
          ],
        },
      },
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    const supported = sessions.find((session) => session.id === "supported-thread");
    const unsupported = sessions.find((session) => session.id === "unsupported-thread");
    assert.equal(supported.model, "gpt-5.5");
    assert.equal(supported.modelSupport, "supported");
    assert.equal(supported.modelSupported, true);
    assert.equal(unsupported.model, "gpt-5.3-codex");
    assert.equal(unsupported.modelSupport, "unsupported");
    assert.equal(unsupported.modelSupported, false);
  });
});

test("resolveActive returns newest started session, not oldest recently-written rollout", async () => {
  await withFakeHome({
    sessions: {
      "2026/06/03": {
        "rollout-2026-06-03T17-05-14-019e8cba-d1d3-7bf2-98e3-2b45d891cea0.jsonl": {
          mtimeAgo: 100,
          meta: { id: "old-long-running", cwd: "/repo" },
        },
        "rollout-2026-06-03T19-32-42-019e8d41-d656-7cf2-86ea-e7140b73a63e.jsonl": {
          mtimeAgo: 60_000,
          meta: { id: "newer-started", cwd: "/repo" },
        },
      },
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const active = await a.resolveActive();
    assert.ok(active);
    assert.equal(active.id, "newer-started");
  });
});

test("resolveActive skips Codex sessions with unsupported models", async () => {
  await withFakeHome({
    modelsCache: {
      models: [
        { slug: "gpt-5.5" },
      ],
    },
    sessions: {
      "2026/06/03": {
        "rollout-2026-06-03T20-00-00-unsupported-thread.jsonl": {
          mtimeAgo: 1_000,
          meta: { id: "unsupported-thread", cwd: "/repo" },
          lines: [
            { type: "turn_context", payload: { model: "gpt-5.3-codex" } },
          ],
        },
        "rollout-2026-06-03T19-30-00-supported-thread.jsonl": {
          mtimeAgo: 60_000,
          meta: { id: "supported-thread", cwd: "/repo" },
          lines: [
            { type: "turn_context", payload: { model: "gpt-5.5" } },
          ],
        },
      },
    },
  }, async (home) => {
    const logs = [];
    const a = makeAdapter(home, {
      log: (level, msg, details) => logs.push({ level, msg, details }),
    });
    const active = await a.resolveActive();
    assert.ok(active);
    assert.equal(active.id, "supported-thread");
    assert.ok(logs.some((entry) => entry.msg === "codex auto skipped unsupported model sessions"));
  });
});

test("resolveActive keeps newest session when model cache is unavailable", async () => {
  await withFakeHome({
    sessions: {
      "2026/06/03": {
        "rollout-2026-06-03T20-00-00-newest-thread.jsonl": {
          mtimeAgo: 1_000,
          meta: { id: "newest-thread", cwd: "/repo" },
          lines: [
            { type: "turn_context", payload: { model: "gpt-5.3-codex" } },
          ],
        },
        "rollout-2026-06-03T19-30-00-older-thread.jsonl": {
          mtimeAgo: 60_000,
          meta: { id: "older-thread", cwd: "/repo" },
          lines: [
            { type: "turn_context", payload: { model: "gpt-5.5" } },
          ],
        },
      },
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const active = await a.resolveActive();
    assert.ok(active);
    assert.equal(active.id, "newest-thread");
    assert.equal(active.modelSupport, "unknown");
  });
});

test("inject yields token+done events from fake codex", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home);
    const events = [];
    for await (const evt of a.inject({ sessionId: "auto", text: "ping" })) {
      events.push(evt);
    }
    const kinds = events.map((e) => e.kind);
    assert.ok(kinds.includes("token"), `no token event: ${JSON.stringify(events)}`);
    assert.equal(kinds[kinds.length - 1], "done");
    const done = events[events.length - 1];
    assert.match(done.sessionId, /^fake-codex-/);
    assert.equal(done.stopReason, "end_turn");
  });
});

test("inject preserves explicit sessionId via app-server resume", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home);
    const events = [];
    for await (const evt of a.inject({ sessionId: "my-codex-sid", text: "ping" })) {
      events.push(evt);
    }
    const done = events[events.length - 1];
    assert.equal(done.sessionId, "my-codex-sid");
  });
});

test("inject with explicit sessionId avoids CLI exec resume model selection", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home, {
      env: {
        FAKE_CODEX_UNSUPPORTED_MODEL_ON_RESUME: "1",
      },
    });
    const events = [];
    for await (const evt of a.inject({ sessionId: "old-session", text: "ping" })) {
      events.push(evt);
    }
    assert.ok(events.some((e) => e.kind === "token" && e.text === "done"), JSON.stringify(events));
    const done = events[events.length - 1];
    assert.equal(done.kind, "done");
    assert.equal(done.sessionId, "old-session");
  });
});

test("inject keeps resume semantics when resumed Codex model is unsupported", async () => {
  await withFakeHome(null, async (home) => {
    const a = new CodexAdapter({
      env: {
        HOME: home,
        PATH: process.env.PATH,
        CODEX_CLI_PATH: fakeCodexEntryForHost(home),
        CLAWD_CODEX_APP_SERVER: "0",
        FAKE_CODEX_UNSUPPORTED_MODEL_ON_RESUME: "1",
      },
      cwd: home,
      fallbackPaths: [],
      extraPathDirs: [],
    });
    const events = [];
    for await (const evt of a.inject({ sessionId: "old-session", text: "ping" })) {
      events.push(evt);
    }
    const errors = events.filter((e) => e.kind === "error");
    assert.equal(errors.length, 1, `unexpected errors: ${JSON.stringify(events)}`);
    assert.equal(errors[0].code, "AGENT_UNSUPPORTED_MODEL");
    assert.match(errors[0].details, /model is not supported/);
    assert.ok(!events.some((e) => e.kind === "done"), `unexpected done: ${JSON.stringify(events)}`);
  });
});

test("inject yields error event when codex exits with FAIL", async () => {
  await withFakeHome(null, async (home) => {
    const a = new CodexAdapter({
      env: {
        HOME: home,
        PATH: process.env.PATH,
        CODEX_CLI_PATH: fakeCodexEntryForHost(home),
        FAKE_CODEX_FAIL: "1",
      },
      cwd: home,
      fallbackPaths: [],
      extraPathDirs: [],
    });
    const events = [];
    for await (const evt of a.inject({ sessionId: "auto", text: "ping" })) {
      events.push(evt);
    }
    const errors = events.filter((e) => e.kind === "error");
    assert.ok(errors.length > 0, `expected error events: ${JSON.stringify(events)}`);
  });
});
