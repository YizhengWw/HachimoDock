"use strict";

/*
 * [Input] Fake Codex homes, JSONL session trees, and shimmed Codex CLI processes.
 * [Output] Regression coverage for Codex adapter session selection, resume handling, and metadata-less rollout skipping.
 * [Pos] Node tests for the agent-session-bus Codex adapter.
 * [Sync] If Codex session filtering or resume behavior changes, update `pc/.folder.md`.
 */

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
    assert.ok(
      a._defaultExtraPathDirs().indexOf(`${appData}\\npm`)
        < a._defaultExtraPathDirs().indexOf(`${localAppData}\\OpenAI\\Codex\\bin`),
    );
  });
});

test("default resolver paths include user-relative macOS desktop runtimes", async () => {
  if (process.platform !== "darwin") return;
  await withFakeHome(null, async (home) => {
    const a = new CodexAdapter({
      env: { HOME: home, PATH: "" },
      cwd: home,
    });
    assert.ok(a._defaultFallbackPaths().includes(
      `${home}/Applications/ChatGPT.app/Contents/Resources/codex`,
    ));
    assert.ok(a._defaultFallbackPaths().includes(
      `${home}/Applications/Codex.app/Contents/Resources/codex`,
    ));
    assert.ok(a._defaultFallbackPaths().every((candidate) => !candidate.startsWith("/Applications/")));
  });
});

test("availability cache keeps the established refresh cadence outside Windows", async () => {
  await withFakeHome(null, async (home) => {
    const adapter = makeAdapter(home);
    assert.equal(
      adapter._availabilityCacheMs,
      process.platform === "win32" ? 30_000 : 5_000,
    );
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

test("listSessions excludes internal Codex approval review rollouts", async () => {
  const approvalPrompt = [
    "The following is the Code under review.",
    "Return risk_level, user_authorization, outcome, and rationale.",
  ].join(" ");
  await withFakeHome({
    sessions: {
      "2026/06/03": {
        "rollout-2026-06-03T19-32-42-internal-review.jsonl": {
          mtimeAgo: 100,
          meta: { id: "internal-review", cwd: "/repo" },
          lines: [{
            type: "response_item",
            payload: {
              type: "message",
              role: "user",
              content: [{ text: approvalPrompt }],
            },
          }],
        },
        "rollout-2026-06-03T19-31-42-user-session.jsonl": {
          mtimeAgo: 1_000,
          meta: { id: "user-session", cwd: "/repo" },
          lines: [{
            type: "response_item",
            payload: {
              type: "message",
              role: "user",
              content: [{ text: "Implement the visible device session card" }],
            },
          }],
        },
      },
    },
  }, async (home) => {
    const sessions = await makeAdapter(home).listSessions();
    assert.deepEqual(sessions.map((session) => session.id), ["user-session"]);
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

test("listSessions bounds initial parsing and reuses unchanged rollout metadata", async () => {
  const sessions = {};
  for (let index = 0; index < 30; index += 1) {
    const suffix = String(index).padStart(12, "0");
    const id = `019f0000-0000-7000-8000-${suffix}`;
    sessions[`rollout-2026-06-03T19-${String(index).padStart(2, "0")}-00-${id}.jsonl`] = {
      mtimeAgo: index * 1_000,
      meta: { id, cwd: "/repo" },
      lines: [
        { type: "turn_context", payload: { model: "gpt-5.5" } },
      ],
    };
  }

  await withFakeHome({
    modelsCache: { models: [{ slug: "gpt-5.5" }] },
    sessions: { "2026/06/03": sessions },
  }, async (home) => {
    const adapter = makeAdapter(home);
    const originalReadSync = fs.readSync;
    let readCount = 0;
    fs.readSync = function countedReadSync(...args) {
      readCount += 1;
      return originalReadSync.apply(this, args);
    };
    try {
      const first = await adapter.listSessions({ limit: 5 });
      assert.equal(first.length, 5);
      assert.ok(readCount <= 10, `expected at most two reads per visible session, saw ${readCount}`);

      const firstReadCount = readCount;
      const second = await adapter.listSessions({ limit: 5 });
      assert.deepEqual(second, first);
      assert.equal(readCount, firstReadCount, "unchanged rollout files should stay parsed in memory");

      const newestPath = path.join(
        home,
        ".codex",
        "sessions",
        "2026",
        "06",
        "03",
        Object.keys(sessions).at(-1),
      );
      fs.appendFileSync(
        newestPath,
        `${JSON.stringify({ type: "turn_context", payload: { model: "gpt-5.5" } })}\n`,
      );
      const third = await adapter.listSessions({ limit: 5 });
      assert.equal(third.length, 5);
      assert.ok(readCount > firstReadCount, "a changed rollout should be reparsed");
      assert.ok(
        readCount <= firstReadCount + 2,
        `only the changed rollout should be reparsed, saw ${readCount - firstReadCount} reads`,
      );
    } finally {
      fs.readSync = originalReadSync;
    }
  });
});

test("listSessions ranks a named canonical id before applying the visible limit", async () => {
  const canonicalId = "019f0000-0000-7000-8000-000000000099";
  const fileId = "019f0000-0000-7000-8000-000000000001";
  const recentId = "019f0000-0000-7000-8000-000000000002";

  await withFakeHome({
    sessionIndex: [
      {
        id: canonicalId,
        thread_name: "Named desktop thread",
        updated_at: new Date().toISOString(),
      },
    ],
    sessions: {
      "2026/06/03": {
        [`rollout-2026-06-03T10-00-00-${fileId}.jsonl`]: {
          mtimeAgo: 60_000,
          meta: { id: canonicalId, cwd: "/repo" },
        },
        [`rollout-2026-06-03T11-00-00-${recentId}.jsonl`]: {
          mtimeAgo: 0,
          meta: { id: recentId, cwd: "/repo" },
        },
      },
    },
  }, async (home) => {
    const adapter = makeAdapter(home);
    const sessions = await adapter.listSessions({ limit: 1 });

    assert.equal(sessions.length, 1);
    assert.equal(sessions[0].id, canonicalId);
    assert.equal(sessions[0].name, "Named desktop thread");
  });
});

test("listSessions collapses multiple rollout files for the same Codex thread id", async () => {
  const canonicalId = "019f0000-0000-7000-8000-000000000099";
  const firstFileId = "019f0000-0000-7000-8000-000000000101";
  const secondFileId = "019f0000-0000-7000-8000-000000000102";

  await withFakeHome({
    sessionIndex: [{
      id: canonicalId,
      thread_name: "One desktop thread",
      updated_at: new Date().toISOString(),
    }],
    sessions: {
      "2026/06/03": {
        [`rollout-2026-06-03T10-00-00-${firstFileId}.jsonl`]: {
          mtimeAgo: 60_000,
          meta: { id: canonicalId, cwd: "/repo", summary: "older" },
        },
        [`rollout-2026-06-03T11-00-00-${secondFileId}.jsonl`]: {
          mtimeAgo: 0,
          meta: { id: canonicalId, cwd: "/repo", summary: "newer" },
        },
      },
    },
  }, async (home) => {
    const adapter = makeAdapter(home);
    const sessions = await adapter.listSessions({ limit: 20 });

    assert.equal(sessions.filter((session) => session.id === canonicalId).length, 1);
    assert.equal(sessions[0].name, "One desktop thread");
    assert.ok(Date.now() - sessions[0].lastModified < 10_000);
  });
});

test("listSessions permanently classifies metadata-less rollouts without rereading them", async () => {
  const namedId = "019f0000-0000-7000-8000-000000000010";
  const internalId = "019f0000-0000-7000-8000-000000000011";
  const staleNamedId = "019f0000-0000-7000-8000-000000000012";
  const internalName = `rollout-2026-06-03T12-00-00-${internalId}.jsonl`;

  await withFakeHome({
    sessionIndex: [
      {
        id: namedId,
        thread_name: "Visible thread",
        updated_at: new Date().toISOString(),
      },
      {
        id: staleNamedId,
        thread_name: "Stale named thread",
        updated_at: new Date().toISOString(),
      },
    ],
    sessions: {
      "2026/06/03": {
        [`rollout-2026-06-03T11-00-00-${namedId}.jsonl`]: {
          meta: { id: namedId, cwd: "/repo" },
        },
        [internalName]: {
          lines: [{ type: "event_msg", payload: { type: "task_started" } }],
        },
      },
    },
  }, async (home) => {
    const adapter = makeAdapter(home);
    const originalReadSync = fs.readSync;
    let readCount = 0;
    fs.readSync = function countedReadSync(...args) {
      readCount += 1;
      return originalReadSync.apply(this, args);
    };
    try {
      const first = await adapter.listSessions({ limit: 1 });
      assert.equal(first[0].id, namedId);
      const firstReadCount = readCount;

      fs.appendFileSync(
        path.join(home, ".codex", "sessions", "2026", "06", "03", internalName),
        `${JSON.stringify({ type: "event_msg", payload: { type: "token_count" } })}\n`,
      );
      const second = await adapter.listSessions({ limit: 1 });
      assert.deepEqual(second, first);
      assert.equal(
        readCount,
        firstReadCount,
        "a rollout whose first complete record is not session_meta is permanently non-resumable",
      );
    } finally {
      fs.readSync = originalReadSync;
    }
  });
});

test("listSessions bounds stale named-index identity scans for large histories", async () => {
  const sessions = {};
  for (let index = 0; index < 1100; index += 1) {
    const suffix = String(index).padStart(12, "0");
    const id = `019f0000-0000-7000-8000-${suffix}`;
    sessions[`rollout-2026-06-03T12-00-${String(index % 60).padStart(2, "0")}-${id}.jsonl`] = {
      mtimeAgo: index * 1_000,
      meta: { id, cwd: "/repo" },
    };
  }

  await withFakeHome({
    sessionIndex: [{
      id: "019f0000-0000-7000-8000-999999999999",
      thread_name: "Deleted desktop thread",
      updated_at: new Date().toISOString(),
    }],
    sessions: { "2026/06/03": sessions },
  }, async (home) => {
    const adapter = makeAdapter(home);
    const originalReadSync = fs.readSync;
    let readCount = 0;
    fs.readSync = function countedReadSync(...args) {
      readCount += 1;
      return originalReadSync.apply(this, args);
    };
    try {
      const first = await adapter.listSessions({ limit: 1 });
      assert.equal(first.length, 1);
      assert.ok(readCount <= 66, `stale index scan read ${readCount} files`);

      const firstReadCount = readCount;
      const second = await adapter.listSessions({ limit: 1 });
      assert.deepEqual(second, first);
      assert.equal(
        readCount,
        firstReadCount,
        "bounded identity candidates should remain cached across polls",
      );
    } finally {
      fs.readSync = originalReadSync;
    }
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

test("resolveActive ignores rollout files without Codex session metadata", async () => {
  await withFakeHome({
    sessions: {
      "2026/06/30": {
        "rollout-2026-06-30T10-58-56-019f1677-2c90-7bd3-933c-35304cc66962.jsonl": {
          mtimeAgo: 100,
        },
        "rollout-2026-06-30T10-40-00-019f1600-0000-7000-8000-000000000001.jsonl": {
          mtimeAgo: 60_000,
          meta: { id: "valid-thread", cwd: "/repo" },
        },
      },
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const active = await a.resolveActive();
    assert.ok(active);
    assert.equal(active.id, "valid-thread");
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

test("managed service tier override is placed before Codex subcommands", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home, { env: { CLAWD_CODEX_SERVICE_TIER: "fast" } });
    assert.deepEqual(
      a._buildExecArgs({ sessionId: "", text: "ping" }).slice(0, 3),
      ["-c", 'service_tier="fast"', "exec"],
    );
    const events = [];
    for await (const evt of a.inject({ sessionId: "managed-session", text: "ping" })) {
      events.push(evt);
    }
    assert.equal(events.at(-1)?.sessionId, "managed-session");
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
