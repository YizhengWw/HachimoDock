"use strict";

const { test } = require("node:test");
const assert = require("node:assert/strict");
const http = require("node:http");

const { createAgentSessionBus, MockAdapter } = require("../src/index");

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

async function startBus() {
  const bus = createAgentSessionBus({
    port: 0, // pick free port
    adapters: [new MockAdapter({ agentId: "mock", tokensPerSecond: 60 })],
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

test("GET /agent/status echoes Origin in CORS headers", async () => {
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
