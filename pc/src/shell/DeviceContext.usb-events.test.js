// Executes the actual provider USB effect with only IPC/events/timers/state doubled.
// No React mount claim, USB port, native process or device operation.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { normalizeUsbStatusPayload, usbStatusSnapshotsEqual } from "./DeviceContext.pure.js";
const source = readFileSync(new URL("./DeviceContext.jsx", import.meta.url), "utf8");
const body = source.match(/\/\/ --- USB status poll[^\n]*\n\s*useEffect\(\(\) => \{([\s\S]*?)\n  \}, \[\]\);/)?.[1];
assert.ok(body, "actual USB effect extraction required");
const effect = new Function("invoke", "listen", "setUsb", "normalizeUsbStatusPayload", "usbStatusSnapshotsEqual", "setTimeout", "clearTimeout", "setInterval", "clearInterval", "USB_STATUS_POLL_MS", "USB_STATUS_SLOW_WARNING_MS", "console", body);
const flush = async () => { for (let n = 0; n < 8; n += 1) await Promise.resolve(); };
function harness() {
  const reads = [], handlers = new Map(), removed = [], state = [];
  let interval, activeReads = 0, peakReads = 0;
  const invoke = (name) => {
    assert.equal(name, "usb_get_status", "effect must never scan/connect/send/reset");
    activeReads += 1; peakReads = Math.max(peakReads, activeReads);
    return new Promise((resolve, reject) => reads.push({ resolve: value => { activeReads -= 1; resolve(value); }, reject: error => { activeReads -= 1; reject(error); } }));
  };
  const listen = (name, handler) => { handlers.set(name, handler); return Promise.resolve(() => { removed.push(name); handlers.delete(name); }); };
  const dispose = effect(invoke, listen, fn => state.push(fn(state.at(-1) || {})), normalizeUsbStatusPayload, usbStatusSnapshotsEqual,
    () => 1, () => {}, fn => { interval = fn; return 2; }, () => {}, 3000, 10000, { warn() {} });
  return { reads, handlers, removed, state, dispose, interval: () => interval(), peak: () => peakReads };
}
async function initialized() {
  const h = harness(); await flush();
  h.reads[0].resolve({ connected: false }); await flush();
  assert.equal(h.reads.length, 2, "subscription catch-up coalesces behind initial query");
  h.reads[1].resolve({ connected: false }); await flush();
  return h;
}
const online = { connected: true, boardDeviceId: "fixture-board", connectionId: "8", bootId: "0123456789abcdef" };
test("HELLO causes immediate native status read, never trusts event identity", async () => {
  const h = await initialized();
  h.handlers.get("usb-message")({ payload: { topic: "hello", payload: { ...online, boardDeviceId: "untrusted-event" } } });
  assert.equal(h.reads.length, 3); assert.equal(h.state.at(-1).connected, false);
  h.reads[2].resolve(online); await flush();
  assert.equal(h.state.at(-1).boardDeviceId, "fixture-board"); assert.equal(h.peak(), 1); h.dispose();
});
test("in-flight stale read is discarded; burst hints coalesce to one follow-up", async () => {
  const h = await initialized(); h.interval();
  const before = h.state.length;
  for (let i = 0; i < 25; i += 1) h.handlers.get("usb-message")({ payload: { topic: "hello" } });
  assert.equal(h.reads.length, 3);
  h.reads[2].resolve({ ...online, connectionId: "7" }); await flush();
  assert.equal(h.state.length, before); assert.equal(h.reads.length, 4);
  h.reads[3].resolve(online); await flush();
  assert.equal(h.state.at(-1).connectionId, "8"); assert.equal(h.peak(), 1); h.dispose();
});
test("disconnect is only a hint and unrelated traffic cannot schedule reads", async () => {
  const h = await initialized();
  h.handlers.get("usb-message")({ payload: { topic: "audio/chunk" } }); assert.equal(h.reads.length, 2);
  h.handlers.get("usb-disconnected")({}); assert.equal(h.reads.length, 3);
  h.reads[2].resolve({ connected: false }); await flush(); assert.equal(h.state.at(-1).connected, false); h.dispose();
});
test("dispose removes both listeners and suppresses pending state/follow-up", async () => {
  const h = await initialized(); h.interval(); h.handlers.get("usb-message")({ payload: { topic: "hello" } });
  const before = h.state.length; h.dispose(); h.reads[2].resolve(online); await flush();
  assert.equal(h.reads.length, 3); assert.equal(h.state.length, before); assert.deepEqual(h.removed.sort(), ["usb-disconnected", "usb-message"]);
});
test("failed reads still unlock polling and retain event-triggered follow-up", async () => {
  const h = await initialized(); h.interval(); h.handlers.get("usb-message")({ payload: { topic: "availability" } });
  h.reads[2].reject(new Error("synthetic failure")); await flush(); assert.equal(h.reads.length, 4);
  h.reads[3].resolve(online); await flush(); assert.equal(h.state.at(-1).connected, true); h.dispose();
});
