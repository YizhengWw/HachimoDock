"use strict";

// End-to-end: run a real ClaudeCodeAdapter (driven by fake-claude.js) behind
// a real AgentSessionBus and exercise the /agent/inject SSE wire from a
// vanilla fetch client. This is the closest we can get to the production
// pipeline without invoking the user's real `claude` CLI:
//
//   fetch → AgentSessionBus → ClaudeCodeAdapter → spawn(fake-claude) → SSE
//
// What it locks in:
//  - HTTP body shape ({ agentId, sessionId, text }) is accepted
//  - SSE event format ("event: token\ndata: {...}\n\n") is emitted
//  - sessionId echoed in the final `done` event matches what fake-claude
//    minted at start.

const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { createAgentSessionBus, ClaudeCodeAdapter } = require("../src/index");

const FAKE_CLAUDE = path.join(__dirname, "fixtures", "fake-claude.js");

test("end-to-end: bus → ClaudeCodeAdapter → fake-claude → SSE", async () => {
  // Use a hermetic HOME so resolveActive() (which walks ~/.claude/projects)
  // doesn't pick up the developer's actual claude session history when
  // running this test on their machine. Without this we'd be asserting
  // against whatever sid the user's real IDE was last attached to.
  const tmpHome = fs.mkdtempSync(path.join(os.tmpdir(), "bus-e2e-"));

  const adapter = new ClaudeCodeAdapter({
    env: {
      ...process.env,
      HOME: tmpHome,
      CLAUDE_CLI_PATH: FAKE_CLAUDE,
      FAKE_CLAUDE_REPLY: "改完了",
    },
    cwd: tmpHome,
    fallbackPaths: [],
    extraPathDirs: [],
  });

  const bus = createAgentSessionBus({ port: 0, host: "127.0.0.1", adapters: [adapter] });
  const port = await bus.start();

  try {
    const resp = await fetch(`http://127.0.0.1:${port}/agent/inject`, {
      method: "POST",
      headers: { "content-type": "application/json", accept: "text/event-stream" },
      body: JSON.stringify({ agentId: "claude-code", sessionId: "auto", text: "ping" }),
    });
    assert.equal(resp.status, 200);

    const decoder = new TextDecoder();
    let buf = "";
    let currentEvent = null;
    let dataLines = [];
    const events = [];

    const flush = () => {
      if (!currentEvent) { dataLines = []; return; }
      const data = dataLines.join("\n");
      dataLines = [];
      try { events.push({ event: currentEvent, data: JSON.parse(data) }); }
      catch { /* ignore non-JSON */ }
      currentEvent = null;
    };

    for await (const chunk of resp.body) {
      buf += decoder.decode(chunk, { stream: true });
      let idx;
      while ((idx = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, idx).replace(/\r$/, "");
        buf = buf.slice(idx + 1);
        if (line === "") { flush(); continue; }
        if (line.startsWith(":")) continue;
        if (line.startsWith("event:")) currentEvent = line.slice(6).trim();
        else if (line.startsWith("data:")) dataLines.push(line.slice(5).replace(/^ /, ""));
      }
    }

    const tokens = events.filter((e) => e.event === "token").map((e) => e.data.text).join("");
    assert.match(tokens, /改完了/, `expected reply tokens, got events: ${JSON.stringify(events)}`);
    const done = events.find((e) => e.event === "done");
    assert.ok(done, `expected a done event in: ${JSON.stringify(events)}`);
    assert.match(done.data.sessionId, /^fake-sid-/);
  } finally {
    await bus.stop();
    fs.rmSync(tmpHome, { recursive: true, force: true });
  }
});
