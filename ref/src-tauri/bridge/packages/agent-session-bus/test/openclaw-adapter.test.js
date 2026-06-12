"use strict";

const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { OpenClawAdapter } = require("../src/adapters/openclaw");
const { locateOpenClaw } = require("../src/util/openclaw-paths");

const FAKE_PKG = path.join(__dirname, "fixtures", "fake-openclaw");
const FAKE_RUNTIME_MODULE = path.join(FAKE_PKG, "dist", "plugin-sdk", "agent-runtime.js");

async function withFakeHome(setup, fn) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "bus-openclaw-test-"));
  if (setup && (setup.sessions || setup.sessionsJson)) {
    const dir = path.join(home, ".openclaw", "agents", setup.agentId || "main", "sessions");
    fs.mkdirSync(dir, { recursive: true });
    if (setup.sessionsJson) {
      fs.writeFileSync(path.join(dir, "sessions.json"), JSON.stringify(setup.sessionsJson));
    }
    if (setup.sessions) {
      for (const [sid, mtimeAgo] of Object.entries(setup.sessions)) {
        const file = path.join(dir, `${sid}.jsonl`);
        fs.writeFileSync(file, "");
        const mtime = (Date.now() - mtimeAgo) / 1000;
        fs.utimesSync(file, mtime, mtime);
      }
    }
  }
  try {
    return await fn(home);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
}

function makeAdapter(home, extra = {}) {
  const { env: extraEnv = {}, ...rest } = extra;
  return new OpenClawAdapter({
    env: {
      HOME: home,
      PATH: process.env.PATH,
      OPENCLAW_RUNTIME_MODULE: FAKE_RUNTIME_MODULE,
      ...extraEnv,
    },
    cwd: home,
    ...rest,
  });
}

test("isAvailable: false when openclaw is not installed", async () => {
  await withFakeHome(null, async (home) => {
    const a = new OpenClawAdapter({
      env: { HOME: home, PATH: "" },
      cwd: home,
    });
    const probe = await a.isAvailable();
    assert.equal(probe.ready, false);
    assert.match(probe.reason, /未安装|未找到/);
  });
});

test("isAvailable: true when OPENCLAW_RUNTIME_MODULE points to fake package", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home);
    const probe = await a.isAvailable();
    assert.equal(probe.ready, true, probe.reason || "should be ready");
    assert.equal(probe.details.packageVersion, "0.0.1-fake");
  });
});

test("isAvailable: true via OPENCLAW_HOME env override", async () => {
  await withFakeHome(null, async (home) => {
    const a = new OpenClawAdapter({
      env: { HOME: home, PATH: "", OPENCLAW_HOME: FAKE_PKG },
      cwd: home,
    });
    const probe = await a.isAvailable();
    assert.equal(probe.ready, true, probe.reason || "should be ready");
  });
});

test("locateOpenClaw checks the Windows npm global package directory", async () => {
  await withFakeHome(null, async (home) => {
    const appData = path.join(home, "AppData", "Roaming");
    const packageRoot = path.join(appData, "npm", "node_modules", "openclaw");
    fs.mkdirSync(path.join(packageRoot, "dist", "plugin-sdk"), { recursive: true });
    fs.writeFileSync(path.join(packageRoot, "package.json"), JSON.stringify({
      name: "openclaw",
      version: "9.9.9-test",
    }));
    fs.writeFileSync(path.join(packageRoot, "dist", "plugin-sdk", "agent-runtime.js"), "");

    const found = locateOpenClaw({
      env: {
        USERPROFILE: home,
        APPDATA: appData,
        PATH: "",
      },
    });
    assert.equal(found?.packageRoot, packageRoot);
    assert.equal(found?.packageVersion, "9.9.9-test");
  });
});

test("listSessions returns [] when ~/.openclaw is missing", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    assert.deepEqual(sessions, []);
  });
});

test("listSessions reads sessions.json when present", async () => {
  await withFakeHome({
    sessionsJson: {
      sessions: [
        { id: "old-sid", updatedAt: 1000 },
        { id: "newest-sid", updatedAt: 9999 },
        { id: "mid-sid", updatedAt: 5000 },
      ],
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    assert.equal(sessions.length, 3);
    assert.equal(sessions[0].id, "newest-sid");
    assert.equal(sessions[2].id, "old-sid");
  });
});

test("listSessions falls back to mtime-walk when sessions.json missing", async () => {
  await withFakeHome({
    sessions: { "old": 60_000, "newest": 1000 },
  }, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    assert.equal(sessions[0].id, "newest");
  });
});

test("inject yields token+done events from fake openclaw", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home, { env: { FAKE_OC_REPLY: "你好世界" } });
    const events = [];
    for await (const evt of a.inject({ sessionId: "auto", text: "ping" })) {
      events.push(evt);
    }
    const kinds = events.map((e) => e.kind);
    assert.ok(kinds.includes("token"), `no token event: ${JSON.stringify(events)}`);
    const tokenText = events.filter((e) => e.kind === "token").map((e) => e.text).join("");
    assert.match(tokenText, /你好世界/);
    assert.equal(kinds[kinds.length - 1], "done");
    const done = events[events.length - 1];
    assert.match(done.sessionId, /^fake-oc-/);
  });
});

test("inject preserves explicit sessionId", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home);
    const events = [];
    for await (const evt of a.inject({ sessionId: "my-oc-sid", text: "ping" })) {
      events.push(evt);
    }
    const done = events[events.length - 1];
    assert.equal(done.sessionId, "my-oc-sid");
  });
});

test("inject yields error event when openclaw throws", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home, { env: { FAKE_OC_FAIL: "1" } });
    const events = [];
    for await (const evt of a.inject({ sessionId: "auto", text: "ping" })) {
      events.push(evt);
    }
    const errors = events.filter((e) => e.kind === "error");
    assert.ok(errors.length > 0, `expected error events: ${JSON.stringify(events)}`);
    assert.match(errors[0].message, /fake openclaw failure/);
  });
});
