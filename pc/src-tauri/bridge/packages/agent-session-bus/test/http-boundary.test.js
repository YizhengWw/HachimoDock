"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const { validateLocalRequest } = require("../src/http-boundary");
const http = require("node:http");
const { createAgentSessionBus, MockAdapter } = require("../src/index");
const request = (headers = {}, extra = {}) => ({
  headers: { host: "127.0.0.1:8181", ...headers },
  socket: { remoteAddress: "127.0.0.1", localPort: 8181 }, url: "/agent/status", ...extra,
});
test("native loopback clients and trusted app remain available", () => {
  assert.equal(validateLocalRequest(request()), null);
  assert.equal(validateLocalRequest(request({ origin: "tauri://localhost" }), { allowAppOrigin: true }), null);
});
test("DNS rebinding, malformed authorities and absolute targets are rejected", () => {
  for (const host of ["attacker.example:8181", "127.0.0.1:80", "127.0.0.1:8181@evil.example", "["]) {
    assert.ok(validateLocalRequest(request({ host })));
  }
  for (const url of ["//attacker.example/agent/status", "http://127.0.0.1:8181/agent/status"]) {
    assert.ok(validateLocalRequest(request({}, { url })));
  }
});
test("web pages cannot forge hooks or read sessions via originless GET", () => {
  assert.ok(validateLocalRequest(request({ origin: "https://attacker.example" })));
  assert.ok(validateLocalRequest(request({ "sec-fetch-site": "cross-site" })));
  assert.ok(validateLocalRequest(request({ "sec-fetch-mode": "navigate" })));
  assert.ok(validateLocalRequest(request({ origin: "tauri://localhost" })));
  assert.ok(validateLocalRequest(request({}, { socket: { remoteAddress: "192.0.2.1", localPort: 8181 } })));
});

test("real HTTP rejects forged Host and browser metadata before dispatch", async () => {
  const bus = createAgentSessionBus({ port: 0, adapters: [new MockAdapter()], log() {} });
  const port = await bus.start();
  const send = (headers) => new Promise((resolve, reject) => {
    const req = http.request({ host: "127.0.0.1", port, method: "POST", path: "/agent/inject",
      headers: { "content-type": "application/json", ...headers } }, res => {
      res.resume(); res.on("end", () => resolve(res.statusCode));
    });
    req.on("error", reject); req.end(JSON.stringify({ agentId: "mock", text: "test only" }));
  });
  try {
    assert.equal(await send({ host: `attacker.example:${port}` }), 403);
    assert.equal(await send({ host: "[" }), 403);
    assert.equal(await send({ "sec-fetch-site": "cross-site" }), 403);
    const health = await fetch(`http://127.0.0.1:${port}/agent/health`);
    assert.equal(health.status, 200);
    await health.json();
  } finally { await bus.stop(); }
});
