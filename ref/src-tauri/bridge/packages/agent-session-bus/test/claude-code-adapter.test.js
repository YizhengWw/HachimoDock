"use strict";

const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { ClaudeCodeAdapter } = require("../src/adapters/claude-code");

const FAKE_CLAUDE = path.join(__dirname, "fixtures", "fake-claude.js");

function fakeClaudeEntryForHost(home) {
  if (process.platform !== "win32") return FAKE_CLAUDE;
  const shim = path.join(home, "fake-claude.cmd");
  const nodePath = process.execPath.replace(/"/g, '""');
  const scriptPath = FAKE_CLAUDE.replace(/"/g, '""');
  fs.writeFileSync(shim, `@echo off\r\n"${nodePath}" "${scriptPath}" %*\r\n`, "utf8");
  return shim;
}

async function withFakeHome(setup, fn) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "bus-claude-test-"));
  // shim a .claude dir if requested
  if (setup && setup.projects) {
    for (const [proj, sessions] of Object.entries(setup.projects)) {
      const dir = path.join(home, ".claude", "projects", proj);
      fs.mkdirSync(dir, { recursive: true });
      for (const [sid, entry] of Object.entries(sessions)) {
        const spec = entry && typeof entry === "object"
          ? entry
          : { mtimeAgo: entry };
        const file = path.join(dir, `${sid}.jsonl`);
        const lines = Array.isArray(spec.lines) ? spec.lines : [];
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
  if (setup && setup.settings) {
    fs.mkdirSync(path.join(home, ".claude"), { recursive: true });
    fs.writeFileSync(path.join(home, ".claude", "settings.json"), "{}");
  }
  try {
    return await fn(home);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
}

function makeAdapter(home, extra = {}) {
  const { env: extraEnv = {}, ...rest } = extra;
  return new ClaudeCodeAdapter({
    env: {
      HOME: home,
      // Need a real PATH so fake-claude.js's `#!/usr/bin/env node` shebang
      // can resolve `node`. Binary discovery is still locked-down via
      // CLAUDE_CLI_PATH + empty fallback/extra below.
      PATH: process.env.PATH,
      CLAUDE_CLI_PATH: fakeClaudeEntryForHost(home),
      ...extraEnv,
    },
    cwd: home,
    fallbackPaths: [],
    extraPathDirs: [],
    ...rest,
  });
}

test("isAvailable: false when CLAUDE_CLI_PATH points to nothing", async () => {
  await withFakeHome(null, async (home) => {
    const a = new ClaudeCodeAdapter({
      // empty PATH + empty fallback/extra → resolver has nothing to fall through to
      env: { HOME: home, PATH: "", CLAUDE_CLI_PATH: "/no/such/file" },
      cwd: home,
      fallbackPaths: [],
      extraPathDirs: [],
    });
    const probe = await a.isAvailable();
    assert.equal(probe.ready, false);
    assert.match(probe.reason, /未找到/);
  });
});

test("isAvailable: true when CLAUDE_CLI_PATH points to fake-claude", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home);
    const probe = await a.isAvailable();
    assert.equal(probe.ready, true, probe.reason || "should be ready");
  });
});

test("isAvailable: false when version is below minVersion", async () => {
  await withFakeHome(null, async (home) => {
    const a = new ClaudeCodeAdapter({
      env: {
        HOME: home,
        PATH: process.env.PATH,
        CLAUDE_CLI_PATH: fakeClaudeEntryForHost(home),
        FAKE_CLAUDE_VERSION: "0.5.0",
      },
      cwd: home,
      minVersion: "1.0.0",
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
    const a = new ClaudeCodeAdapter({
      env: {
        USERPROFILE: home,
        APPDATA: appData,
        LOCALAPPDATA: localAppData,
        PATH: "",
      },
      cwd: home,
    });
    assert.ok(a._defaultFallbackPaths().includes(`${localAppData}\\Microsoft\\WindowsApps\\claude.exe`));
    assert.ok(a._defaultExtraPathDirs().includes(`${appData}\\npm`));
  });
});

test("listSessions returns [] when ~/.claude/projects does not exist", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    assert.deepEqual(sessions, []);
  });
});

test("listSessions returns sessions sorted newest-first", async () => {
  await withFakeHome({
    projects: {
      "-tmp-proj1": { "sid-old": 60_000, "sid-new": 1_000 },
      "-tmp-proj2": { "sid-mid": 5_000 },
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    assert.equal(sessions.length, 3);
    assert.equal(sessions[0].id, "sid-new");
    assert.equal(sessions[2].id, "sid-old");
  });
});

test("listSessions prefers Claude client session names", async () => {
  await withFakeHome({
    projects: {
      "-tmp-proj": {
        "named-sid": {
          mtimeAgo: 1_000,
          lines: [
            { type: "system", cwd: "/tmp/proj", session_name: "修 Windows 语音链路" },
            {
              type: "user",
              cwd: "/tmp/proj",
              message: {
                role: "user",
                content: [{ type: "text", text: "这个不应该盖过客户端标题" }],
              },
            },
          ],
        },
      },
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    assert.equal(sessions[0].name, "修 Windows 语音链路");
    assert.equal(sessions[0].summary, "这个不应该盖过客户端标题");
    assert.equal(sessions[0].cwd, "/tmp/proj");
  });
});

test("listSessions skips Claude bootstrap context before summary fallback", async () => {
  await withFakeHome({
    projects: {
      "-tmp-proj": {
        "summary-sid": {
          mtimeAgo: 1_000,
          lines: [
            {
              type: "user",
              cwd: "/tmp/proj",
              message: {
                role: "user",
                content: [{ type: "text", text: "# AGENTS.md instructions for /tmp/proj" }],
              },
            },
            {
              type: "user",
              cwd: "/tmp/proj",
              message: {
                role: "user",
                content: [{ type: "text", text: "<environment_context>\n  <cwd>/tmp/proj</cwd>" }],
              },
            },
            {
              type: "user",
              cwd: "/tmp/proj",
              message: {
                role: "user",
                content: [{ type: "text", text: "继续修 Claude Code session 列表" }],
              },
            },
          ],
        },
      },
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const sessions = await a.listSessions();
    assert.equal(sessions[0].name, undefined);
    assert.equal(sessions[0].summary, "继续修 Claude Code session 列表");
  });
});

test("resolveActive returns newest session (永远续最近 rule)", async () => {
  await withFakeHome({
    projects: {
      "-tmp-proj": { "old": 86_400_000, "newest": 1000 },
    },
  }, async (home) => {
    const a = makeAdapter(home);
    const active = await a.resolveActive();
    assert.ok(active);
    assert.equal(active.id, "newest");
  });
});

test("inject yields token+done events from fake claude", async () => {
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
    assert.match(done.sessionId, /^fake-sid-/);
    assert.equal(done.stopReason, "end_turn");
  });
});

test("inject preserves explicit sessionId via --resume", async () => {
  await withFakeHome(null, async (home) => {
    const a = makeAdapter(home);
    const events = [];
    for await (const evt of a.inject({ sessionId: "my-existing-sid", text: "ping" })) {
      events.push(evt);
    }
    const done = events[events.length - 1];
    assert.equal(done.sessionId, "my-existing-sid");
  });
});

test("inject yields error event when claude exits with FAIL", async () => {
  await withFakeHome(null, async (home) => {
    const a = new ClaudeCodeAdapter({
      env: {
        HOME: home,
        PATH: process.env.PATH,
        CLAUDE_CLI_PATH: fakeClaudeEntryForHost(home),
        FAKE_CLAUDE_FAIL: "1",
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
