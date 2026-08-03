/**
 * [Input] bridge-runtime.js readiness gate.
 * [Output] Regression coverage for concurrent-call coalescing, ready-result TTL, and failure retries.
 * [Pos] test node in ref/src/lib
 */

import assert from "node:assert/strict";
import test from "node:test";
import { createBridgeRuntimeEnsurer } from "./bridge-runtime.js";

test("bridge readiness shares concurrent non-forced checks", async () => {
  let resolveInvoke;
  let calls = 0;
  const invokeCommand = () => {
    calls += 1;
    return new Promise((resolve) => {
      resolveInvoke = resolve;
    });
  };
  const ensure = createBridgeRuntimeEnsurer({ invokeCommand });

  const first = ensure();
  const second = ensure();

  assert.equal(first, second);
  await Promise.resolve();
  assert.equal(calls, 1);
  resolveInvoke({ running: true, pid: 42 });
  assert.deepEqual(await first, { running: true, pid: 42 });
});

test("bridge readiness briefly reuses only confirmed-ready results", async () => {
  let currentTime = 1000;
  let calls = 0;
  const ensure = createBridgeRuntimeEnsurer({
    now: () => currentTime,
    readyCacheMs: 5000,
    invokeCommand: async () => {
      calls += 1;
      return { running: true, pid: calls };
    },
  });

  assert.equal((await ensure()).pid, 1);
  currentTime += 4999;
  assert.equal((await ensure()).pid, 1);
  assert.equal(calls, 1);

  currentTime += 2;
  assert.equal((await ensure()).pid, 2);
  assert.equal(calls, 2);
});

test("bridge readiness retries false and rejected results", async () => {
  const outcomes = [
    { running: false },
    new Error("port unavailable"),
    { running: true, pid: 7 },
  ];
  let calls = 0;
  const ensure = createBridgeRuntimeEnsurer({
    invokeCommand: async () => {
      const outcome = outcomes[calls];
      calls += 1;
      if (outcome instanceof Error) throw outcome;
      return outcome;
    },
  });

  assert.equal((await ensure()).running, false);
  await assert.rejects(ensure(), /port unavailable/);
  assert.equal((await ensure()).pid, 7);
  assert.equal(calls, 3);
});

test("forced bridge readiness bypasses the ready cache", async () => {
  let calls = 0;
  const ensure = createBridgeRuntimeEnsurer({
    invokeCommand: async (_command, { input }) => {
      calls += 1;
      return { running: true, forceRestart: input.forceRestart, pid: calls };
    },
  });

  assert.equal((await ensure()).pid, 1);
  const forced = await ensure({ forceRestart: true });

  assert.equal(forced.pid, 2);
  assert.equal(forced.forceRestart, true);
});
