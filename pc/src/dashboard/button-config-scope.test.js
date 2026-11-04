import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

// Run the production callback body, with transport/setState boundaries stubbed.
// No copied decision logic, React mount, Tauri invocation or physical device.
const source = readFileSync(new URL("../DeviceDashboard.jsx", import.meta.url), "utf8");
const prefix = "const refreshButtonConfigFromBoard = useCallback(async () => {";
const start = source.indexOf(prefix);
assert.ok(start >= 0);
const end = source.indexOf("\n  }, [", start);
assert.ok(end > start);
const body = source.slice(start + prefix.length, end);

function fixture({ matches = false } = {}) {
  let acknowledge, started;
  const entered = new Promise(resolve => { started = resolve; });
  const ack = new Promise(resolve => { acknowledge = resolve; });
  const events = [], writes = [];
  const queryToken = { current: 0 }, revision = { current: 1 }, target = { current: "board/connection/boot" };
  const bindings = {
    usb: { connected: true, runtime: "esp-p4" }, hasKnownRuntime: true,
    buttonConfigTargetBoardDeviceId: "board", buttonConfigQueryTokenRef: queryToken,
    buttonConfigRevisionRef: revision, voiceTarget: { key: target.current }, voiceTargetKeyRef: target,
    voiceConfigRef: { current: { enabled: true } }, normalizeVoiceConfig: x => x,
    resolveButtonConfigForRuntime: config => ({ config, voiceButton: "sw2", voiceEnabled: true }),
    dispatchBoardButtonConfig: async input => { writes.push(input); started(); return ack; },
    invoke: async command => { assert.equal(command, "usb_get_button_config"); return {}; },
    boardButtonConfigMatchesClient: () => matches,
    setVoiceConfigOtaState: x => events.push(["state", x]),
    setVoiceConfigDirty: x => events.push(["dirty", x]),
    setButtonConfigHydratedFor: x => events.push(["hydrated", x]),
  };
  const run = new Function(...Object.keys(bindings), `return async function () {${body}\n}`)(...Object.values(bindings));
  return { run, entered, acknowledge, events, writes, queryToken, revision, target };
}

for (const change of ["query", "revision", "connection"]) {
  test(`old button ACK cannot publish success or hydration after ${change} changes`, async () => {
    const f = fixture();
    const pending = f.run();
    await f.entered;
    assert.equal(f.writes.length, 1);
    if (change === "query") f.queryToken.current++;
    if (change === "revision") f.revision.current++;
    if (change === "connection") f.target.current = "board/new-connection/new-boot";
    const count = f.events.length;
    f.acknowledge({ ok: true });
    await pending;
    assert.equal(f.events.length, count, "retired ACK must not alter the new owner's UI");
    assert.equal(f.writes.length, 1, "no mutation replay");
  });
}

test("current button ACK publishes success once and hydrates the current board", async () => {
  const f = fixture();
  const pending = f.run();
  await f.entered;
  f.acknowledge({ ok: true });
  await pending;
  assert.equal(f.writes.length, 1);
  assert.equal(f.events.filter(([type, value]) => type === "state" && value.tone === "success").length, 1);
  assert.deepEqual(f.events.at(-1), ["hydrated", "board"]);
});

test("a matching current button map is read-only and hydrates without rewriting", async () => {
  const f = fixture({ matches: true });
  await f.run();
  assert.equal(f.writes.length, 0);
  assert.deepEqual(f.events.at(-1), ["hydrated", "board"]);
});
