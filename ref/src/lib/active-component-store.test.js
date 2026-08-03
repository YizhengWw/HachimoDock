/**
 * [Input] Versioned active-component storage helpers.
 * [Output] Node coverage for per-device isolation, legacy-safe reads, exact-source
 *          persistence, and target-scoped removal.
 * [Pos] shared component lifecycle store test in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  ACTIVE_COMPONENT_STORAGE_KEY,
  activeComponentTargetKey,
  readActiveComponentForTarget,
  removeActiveComponentForTarget,
  writeActiveComponentForTarget,
} from "./active-component-store.js";

function memoryStorage(initial = {}) {
  const values = new Map(Object.entries(initial));
  return {
    getItem: (key) => values.get(key) ?? null,
    setItem: (key, value) => values.set(key, String(value)),
    removeItem: (key) => values.delete(key),
  };
}

test("active components are isolated by explicit USB and SSH targets", () => {
  const storage = memoryStorage();
  const usb = { transport: "usb", boardDeviceId: "board-a" };
  const ssh = { transport: "ssh", sshHost: "pet@10.0.0.8" };
  writeActiveComponentForTarget({ id: "tomato-clock", name: "番茄钟" }, usb, storage);
  writeActiveComponentForTarget({ id: "token-usage", name: "Token" }, ssh, storage);

  assert.equal(readActiveComponentForTarget(usb, storage).id, "tomato-clock");
  assert.equal(readActiveComponentForTarget(ssh, storage).id, "token-usage");
  assert.equal(readActiveComponentForTarget(
    { transport: "usb", boardDeviceId: "board-b" },
    storage,
  ), null);
});

test("legacy global records are unverified and never gain an inferred target", () => {
  const storage = memoryStorage({
    [ACTIVE_COMPONENT_STORAGE_KEY]: JSON.stringify({ id: "token-usage", name: "Token" }),
  });
  const record = readActiveComponentForTarget(
    { transport: "usb", boardDeviceId: "board-a" },
    storage,
  );
  assert.equal(record.id, "token-usage");
  assert.equal(record.targetVerified, false);
  assert.equal(record.target, null);
});

test("draft source identity survives persistence and removal only clears one target", () => {
  const storage = memoryStorage();
  const usbA = { transport: "usb", boardDeviceId: "board-a" };
  const usbB = { transport: "usb", boardDeviceId: "board-b" };
  writeActiveComponentForTarget(
    { id: "timer", name: "A", isDraft: true, draftPath: "/drafts/a/timer" },
    usbA,
    storage,
  );
  writeActiveComponentForTarget({ id: "timer", name: "B" }, usbB, storage);

  assert.deepEqual(readActiveComponentForTarget(usbA, storage).source, {
    type: "draft",
    path: "/drafts/a/timer",
  });
  assert.equal(removeActiveComponentForTarget(usbA, storage), true);
  assert.equal(readActiveComponentForTarget(usbA, storage), null);
  assert.equal(readActiveComponentForTarget(usbB, storage).name, "B");
  assert.equal(activeComponentTargetKey(usbB), "usb:board-b");
});

test("formal local components persist their content-addressed library source", () => {
  const storage = memoryStorage();
  const target = { transport: "usb", boardDeviceId: "board-library" };
  writeActiveComponentForTarget(
    {
      id: "timer",
      name: "Timer",
      isLocal: true,
      libraryPath: "/home/me/.claw-pet/components/library/timer/0123456789abcdef",
    },
    target,
    storage,
  );

  assert.deepEqual(readActiveComponentForTarget(target, storage).source, {
    type: "library",
    path: "/home/me/.claw-pet/components/library/timer/0123456789abcdef",
  });
});
