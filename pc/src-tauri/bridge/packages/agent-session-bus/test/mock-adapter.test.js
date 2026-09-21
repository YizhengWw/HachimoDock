"use strict";

const { test } = require("node:test");
const assert = require("node:assert/strict");

const { MockAdapter } = require("../src/adapters/mock");

test("mock isAvailable", async () => {
  const m = new MockAdapter();
  const probe = await m.isAvailable();
  assert.equal(probe.ready, true);
});

test("mock listSessions sorted limit", async () => {
  const m = new MockAdapter();
  const all = await m.listSessions();
  assert.equal(all.length, 2);
  const limited = await m.listSessions({ limit: 1 });
  assert.equal(limited.length, 1);
});

test("mock resolveActive returns newest", async () => {
  const m = new MockAdapter();
  const active = await m.resolveActive();
  assert.equal(active.id, "mock-session-001");
});

test("mock openNew prepends a session", async () => {
  const m = new MockAdapter();
  const fresh = await m.openNew();
  const list = await m.listSessions();
  assert.equal(list[0].id, fresh.id);
});

test("mock inject yields tokens then done", async () => {
  const m = new MockAdapter({ tokensPerSecond: 60 });
  const events = [];
  for await (const evt of m.inject({ sessionId: "auto", text: "你好" })) {
    events.push(evt);
  }
  const kinds = events.map((e) => e.kind);
  assert.ok(kinds.includes("token"), "should emit at least one token");
  assert.equal(kinds[kinds.length - 1], "done");
  const done = events[events.length - 1];
  assert.equal(typeof done.sessionId, "string");
  assert.equal(done.stopReason, "end_turn");
});

test("mock inject SESSION_NOT_FOUND for explicit unknown sid", async () => {
  const m = new MockAdapter();
  const events = [];
  for await (const evt of m.inject({ sessionId: "does-not-exist", text: "hi" })) {
    events.push(evt);
  }
  assert.equal(events.length, 1);
  assert.equal(events[0].kind, "error");
  assert.equal(events[0].code, "SESSION_NOT_FOUND");
});

test("mock inject honours abort signal", async () => {
  const m = new MockAdapter({ tokensPerSecond: 4 });
  const ac = new AbortController();
  const events = [];
  const consume = (async () => {
    for await (const evt of m.inject({ sessionId: "auto", text: "long-ish reply", signal: ac.signal })) {
      events.push(evt);
      if (events.length === 1) ac.abort();
    }
  })();
  await consume;
  const lastErr = events.find((e) => e.kind === "error");
  assert.ok(lastErr, `expected an error event, got: ${JSON.stringify(events)}`);
  assert.equal(lastErr.code, "CANCELLED");
});
