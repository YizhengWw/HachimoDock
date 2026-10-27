import test from "node:test";
import assert from "node:assert/strict";
import { createStatusReconciler, mergeStatus } from "./status-reconciler.js";

test("slow polling is single-flight and cannot replace a newer push", async () => {
  const gate = createStatusReconciler();
  let resolve, reads = 0;
  const applied = [];
  const read = () => { reads++; return new Promise(done => { resolve = done; }); };
  const pending = gate.poll(read, x => applied.push(x));
  await gate.poll(read, x => applied.push(x));
  assert.equal(reads, 1);
  gate.invalidate();
  resolve({ sessionId: "old", active: true });
  await pending;
  assert.deepEqual(applied, []);
  await gate.poll(async () => ({ sessionId: "new", active: false }), x => applied.push(x));
  assert.deepEqual(applied, [{ sessionId: "new", active: false }]);
});

test("errors release the poll lock; disposed callbacks never update state", async () => {
  const gate = createStatusReconciler();
  await assert.rejects(gate.poll(async () => { throw Error("offline"); }, () => {}));
  let resolve, applied = 0;
  const pending = gate.poll(() => new Promise(done => { resolve = done; }), () => applied++);
  gate.dispose(); resolve({ active: true }); await pending;
  await gate.poll(async () => ({ active: true }), () => applied++);
  assert.equal(applied, 0);
});

test("identical snapshots retain reference; empty final sessions can clear old state", () => {
  const current = { sessionId: "old", active: true, error: "", details: { count: 1 } };
  assert.equal(mergeStatus(current, { active: true, details: { count: 1 } }), current);
  assert.deepEqual(mergeStatus(current, { sessionId: "", active: false }),
    { ...current, sessionId: "", active: false });
});
