"use strict";

const { test } = require("node:test");
const assert = require("node:assert/strict");

const { AdapterRegistry } = require("../src/registry");
const { BaseAdapter } = require("../src/adapters/base");
const { MockAdapter } = require("../src/adapters/mock");

class FailingAdapter extends BaseAdapter {
  constructor() { super({ agentId: "failing" }); }
  async isAvailable() {
    throw new Error("kaboom");
  }
}

test("register & get", () => {
  const r = new AdapterRegistry();
  const a = new MockAdapter({ agentId: "m1" });
  r.register(a);
  assert.equal(r.has("m1"), true);
  assert.equal(r.get("m1"), a);
  assert.deepEqual(r.ids(), ["m1"]);
});

test("duplicate register throws", () => {
  const r = new AdapterRegistry({ adapters: [new MockAdapter({ agentId: "m1" })] });
  assert.throws(() => r.register(new MockAdapter({ agentId: "m1" })), /already registered/);
});

test("statusOne caches results", async () => {
  const a = new MockAdapter({ agentId: "m1" });
  let calls = 0;
  a.isAvailable = async () => { calls += 1; return { ready: true }; };
  const r = new AdapterRegistry({ adapters: [a] });

  const s1 = await r.statusOne("m1");
  const s2 = await r.statusOne("m1");
  assert.equal(calls, 1);
  assert.equal(s1.ready, true);
  assert.equal(s2.ready, true);

  const s3 = await r.statusOne("m1", { fresh: true });
  assert.equal(calls, 2);
  assert.equal(s3.ready, true);
});

test("statusOne traps thrown isAvailable", async () => {
  const r = new AdapterRegistry({ adapters: [new FailingAdapter()] });
  const s = await r.statusOne("failing");
  assert.equal(s.ready, false);
  assert.match(s.reason, /kaboom/);
});

test("statusAll returns one entry per adapter", async () => {
  const r = new AdapterRegistry({
    adapters: [
      new MockAdapter({ agentId: "a" }),
      new MockAdapter({ agentId: "b" }),
    ],
  });
  const out = await r.statusAll();
  assert.equal(out.length, 2);
  assert.deepEqual(out.map((s) => s.agentId).sort(), ["a", "b"]);
  assert.ok(out.every((s) => s.ready === true));
});
