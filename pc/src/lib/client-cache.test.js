/**
 * [Input] Async client-side loader functions and explicit refresh signals.
 * [Output] Node test coverage for TTL reuse, synchronous peeks, force refresh, invalidation, and pending-load coalescing.
 * [Pos] test node in pc/src/lib
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";

import { createAsyncCache } from "./client-cache.js";

test("createAsyncCache reuses loaded values within the TTL", async () => {
  let now = 1000;
  let calls = 0;
  const cache = createAsyncCache({ ttlMs: 500, now: () => now });
  const loader = async () => {
    calls += 1;
    return [`value-${calls}`];
  };

  assert.deepEqual(await cache.get(loader), ["value-1"]);
  assert.deepEqual(await cache.get(loader), ["value-1"]);
  assert.equal(calls, 1);

  now = 1601;
  assert.deepEqual(await cache.get(loader), ["value-2"]);
  assert.equal(calls, 2);
});

test("createAsyncCache supports force refresh and explicit invalidation", async () => {
  let calls = 0;
  const cache = createAsyncCache({ ttlMs: 30_000, now: () => 2000 });
  const loader = async () => {
    calls += 1;
    return { calls };
  };

  assert.deepEqual(await cache.get(loader), { calls: 1 });
  assert.deepEqual(await cache.get(loader, { force: true }), { calls: 2 });
  cache.invalidate();
  assert.deepEqual(await cache.get(loader), { calls: 3 });
});

test("createAsyncCache exposes a fresh cached value synchronously", async () => {
  let now = 4000;
  let calls = 0;
  const cache = createAsyncCache({ ttlMs: 500, now: () => now });
  const loader = async () => {
    calls += 1;
    return { calls };
  };

  assert.equal(cache.peek(), undefined);
  assert.deepEqual(await cache.get(loader), { calls: 1 });
  assert.deepEqual(cache.peek(), { calls: 1 });

  now = 4601;
  assert.equal(cache.peek(), undefined);
  assert.deepEqual(await cache.get(loader), { calls: 2 });
  cache.invalidate();
  assert.equal(cache.peek(), undefined);
});

test("createAsyncCache coalesces concurrent reads", async () => {
  let calls = 0;
  let release;
  const cache = createAsyncCache({ ttlMs: 30_000, now: () => 3000 });
  const blocker = new Promise((resolve) => {
    release = resolve;
  });
  const loader = async () => {
    calls += 1;
    await blocker;
    return "loaded";
  };

  const first = cache.get(loader);
  const second = cache.get(loader);
  release();

  assert.equal(await first, "loaded");
  assert.equal(await second, "loaded");
  assert.equal(calls, 1);
});

function deferred() {
  let resolve, reject;
  const promise = new Promise((ok, fail) => { resolve = ok; reject = fail; });
  return { promise, resolve, reject };
}

test("invalidated requests cannot restore stale cache entries", async () => {
  const cache = createAsyncCache();
  const old = deferred();
  const first = cache.get(() => old.promise);
  await Promise.resolve();
  cache.invalidate();
  await cache.get(async () => "new");
  old.resolve("old");
  assert.equal(await first, "old");
  assert.equal(cache.peek(), "new");
});

test("invalidation alone retires an in-flight result", async () => {
  const cache = createAsyncCache(), old = deferred();
  const first = cache.get(() => old.promise);
  cache.invalidate();
  old.resolve("old");
  await first;
  assert.equal(cache.peek(), undefined);
});

test("force refresh keeps the most recently requested result", async () => {
  const cache = createAsyncCache(), old = deferred(), fresh = deferred();
  const first = cache.get(() => old.promise);
  const second = cache.get(() => fresh.promise, { force: true });
  fresh.resolve("new"); await second;
  old.resolve("old"); await first;
  assert.equal(cache.peek(), "new");
});

for (const rejected of [false, true]) {
  test(`a retired ${rejected ? "rejected" : "resolved"} request cannot clear the current pending read`, async () => {
    const cache = createAsyncCache(), old = deferred(), fresh = deferred();
    const first = cache.get(() => old.promise).catch(() => undefined);
    const second = cache.get(() => fresh.promise, { force: true });
    if (rejected) old.reject(Error("old failure")); else old.resolve("old");
    await first;
    let extraReads = 0;
    const third = cache.get(async () => { extraReads++; return "unexpected"; });
    fresh.resolve("new");
    assert.deepEqual(await Promise.all([second, third]), ["new", "new"]);
    assert.equal(extraReads, 0);
  });
}
