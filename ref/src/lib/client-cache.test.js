/**
 * [Input] Async client-side loader functions and explicit refresh signals.
 * [Output] Node test coverage for TTL reuse, synchronous peeks, force refresh, invalidation, and pending-load coalescing.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
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
