"use strict";

/*
 * [Input] A local fake DSH bridge plugin HTTP server speaking the contract in docs/dsh-integration-contract.md §2.
 * [Output] Node regression coverage for DshAdapter health probing, session mapping, SSE inject translation, and abort handling.
 * [Pos] Node tests for the agent-session-bus DeepSeek Harness adapter.
 */

const test = require("node:test");
const assert = require("node:assert/strict");
const http = require("node:http");

const { DshAdapter } = require("../src/adapters/dsh");

function startFakePlugin(handler) {
  return new Promise((resolve, reject) => {
    const server = http.createServer((req, res) => {
      handler(req, res).catch((error) => {
        res.destroy(error);
      });
    });
    server.on("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const { port } = server.address();
      resolve({
        baseUrl: `http://127.0.0.1:${port}`,
        close: () => new Promise((done) => server.close(done)),
      });
    });
  });
}

async function withFakePlugin(handler, fn) {
  const plugin = await startFakePlugin(handler);
  try {
    return await fn(plugin);
  } finally {
    await plugin.close();
  }
}

async function readRequestBody(req) {
  let body = "";
  req.setEncoding("utf8");
  for await (const chunk of req) body += chunk;
  return body;
}

function writeSse(res, frames) {
  res.writeHead(200, {
    "content-type": "text/event-stream",
    "cache-control": "no-cache",
  });
  for (const [event, data] of frames) {
    res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);
  }
  res.end();
}

async function collectInjectEvents(adapter, request) {
  const events = [];
  for await (const event of adapter.inject(request)) events.push(event);
  return events;
}

test("base URL defaults to the contract port and honours HACHIMODOCK_DSH_URL", () => {
  assert.equal(new DshAdapter({ env: {} }).baseUrl, "http://127.0.0.1:17892");
  assert.equal(
    new DshAdapter({ env: { HACHIMODOCK_DSH_URL: "http://127.0.0.1:19001/" } }).baseUrl,
    "http://127.0.0.1:19001",
  );
  assert.equal(
    new DshAdapter({ env: { HACHIMODOCK_DSH_URL: "not a url" } }).baseUrl,
    "http://127.0.0.1:17892",
  );
});

test("isAvailable reports false when the fake plugin is not ok", async () => {
  await withFakePlugin((req, res) => {
    assert.equal(req.url, "/health");
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: false, error: "not initialized" }));
    return Promise.resolve();
  }, async (plugin) => {
    const adapter = new DshAdapter({ env: {}, baseUrl: plugin.baseUrl });
    const probe = await adapter.isAvailable();
    assert.equal(probe.ready, false);
    assert.match(probe.reason, /DSH 桥接插件未就绪: not initialized/);
  });
});

test("isAvailable reports true from /health and keeps the protocol version", async () => {
  await withFakePlugin((req, res) => {
    assert.equal(req.url, "/health");
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: true, version: 1, sessions: 2, pendingApprovals: 0 }));
    return Promise.resolve();
  }, async (plugin) => {
    const adapter = new DshAdapter({ env: {}, baseUrl: plugin.baseUrl });
    assert.deepEqual(await adapter.isAvailable(), { ready: true, reason: null, version: 1 });
  });
});

test("isAvailable fails closed when nothing listens on the DSH URL", async () => {
  const plugin = await startFakePlugin(() => Promise.resolve());
  const baseUrl = plugin.baseUrl;
  await plugin.close();
  const probe = await new DshAdapter({ env: {}, baseUrl }).isAvailable();
  assert.equal(probe.ready, false);
  assert.match(probe.reason, /无法连接 DSH 桥接插件/);
});

test("listSessions maps /sessions payloads newest-first", async () => {
  await withFakePlugin((req, res) => {
    assert.equal(req.url, "/sessions");
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({
      sessions: [
        {
          id: "older-session",
          title: "旧会话",
          cwd: "/tmp/old",
          lastModified: 100,
          status: "idle",
          agentId: "deepseek-harness",
        },
        {
          id: "newer-session",
          title: "新会话",
          cwd: "/tmp/new",
          lastModified: 200,
          status: "running",
          displayContent: "### 标题\n**粗体**",
          agentId: "deepseek-harness",
        },
        { id: "", title: "missing id is ignored" },
      ],
    }));
    return Promise.resolve();
  }, async (plugin) => {
    const adapter = new DshAdapter({ env: {}, baseUrl: plugin.baseUrl });
    assert.deepEqual(await adapter.listSessions({ limit: 10 }), [
      {
        id: "newer-session",
        lastModified: 200,
        name: "新会话",
        summary: "新会话",
        displayContent: "### 标题\n**粗体**",
        cwd: "/tmp/new",
      },
      {
        id: "older-session",
        lastModified: 100,
        name: "旧会话",
        summary: "旧会话",
        cwd: "/tmp/old",
      },
    ]);
    assert.equal((await adapter.listSessions({ limit: 1 })).length, 1);
  });
});

test("listSessions returns [] when the plugin is unreachable", async () => {
  const plugin = await startFakePlugin(() => Promise.resolve());
  const baseUrl = plugin.baseUrl;
  await plugin.close();
  const adapter = new DshAdapter({ env: {}, baseUrl });
  assert.deepEqual(await adapter.listSessions(), []);
});

test("inject maps DSH SSE token/tool/done frames into AgentEvents", async () => {
  const requests = [];
  await withFakePlugin(async (req, res) => {
    requests.push({ method: req.method, url: req.url, body: await readRequestBody(req) });
    if (req.url !== "/inject") {
      res.writeHead(404, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: false }));
      return;
    }
    writeSse(res, [
      ["token", { text: "你" }],
      ["tool", { name: "bash", phase: "start", input: { cmd: "ls" }, ok: true }],
      ["token", { text: "好" }],
      ["done", { sessionId: "session-42", tokens: 2 }],
    ]);
  }, async (plugin) => {
    const adapter = new DshAdapter({ env: {}, baseUrl: plugin.baseUrl });
    const events = await collectInjectEvents(adapter, { sessionId: "auto", text: "你好" });
    assert.deepEqual(events, [
      { kind: "token", text: "你" },
      { kind: "tool", name: "bash", phase: "start", input: { cmd: "ls" }, ok: true },
      { kind: "token", text: "好" },
      { kind: "done", sessionId: "session-42", tokens: 2 },
    ]);
    assert.equal(requests.length, 1);
    assert.equal(requests[0].method, "POST");
    assert.deepEqual(JSON.parse(requests[0].body), { sessionId: "auto", text: "你好" });
  });
});

test("inject surfaces DSH SSE error frames as a single terminal error event", async () => {
  await withFakePlugin(async (req, res) => {
    await readRequestBody(req);
    writeSse(res, [
      ["error", { code: "SESSION_NOT_FOUND", message: "no such session" }],
    ]);
  }, async (plugin) => {
    const adapter = new DshAdapter({ env: {}, baseUrl: plugin.baseUrl });
    const events = await collectInjectEvents(adapter, { sessionId: "missing", text: "hi" });
    assert.deepEqual(events, [
      { kind: "error", code: "SESSION_NOT_FOUND", message: "no such session" },
    ]);
  });
});

test("inject reports HTTP failures from the plugin as AGENT_UNAVAILABLE", async () => {
  await withFakePlugin(async (req, res) => {
    await readRequestBody(req);
    res.writeHead(503, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: false, error: "plugin busy" }));
  }, async (plugin) => {
    const adapter = new DshAdapter({ env: {}, baseUrl: plugin.baseUrl });
    const events = await collectInjectEvents(adapter, { sessionId: "auto", text: "hi" });
    assert.equal(events.length, 1);
    assert.equal(events[0].kind, "error");
    assert.equal(events[0].code, "AGENT_UNAVAILABLE");
    assert.match(events[0].message, /HTTP 503/);
  });
});

test("inject honours an already-aborted signal without calling the plugin", async () => {
  let requestCount = 0;
  await withFakePlugin((_req, res) => {
    requestCount += 1;
    res.writeHead(200).end();
    return Promise.resolve();
  }, async (plugin) => {
    const adapter = new DshAdapter({ env: {}, baseUrl: plugin.baseUrl });
    const controller = new AbortController();
    controller.abort();
    const events = await collectInjectEvents(adapter, {
      sessionId: "auto",
      text: "hi",
      signal: controller.signal,
    });
    assert.deepEqual(events, [
      { kind: "error", code: "CANCELLED", message: "DSH inject aborted" },
    ]);
  });
  assert.equal(requestCount, 0);
});

test("openNew defers session creation to the DSH host", async () => {
  const adapter = new DshAdapter({ env: {} });
  const ref = await adapter.openNew({ cwd: "/tmp/work" });
  assert.equal(ref.id, "");
  assert.equal(ref.cwd, "/tmp/work");
  assert.match(ref.summary, /new \(deepseek-harness\)/);
});
