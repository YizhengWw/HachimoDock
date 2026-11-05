/**
 * Production follow workflow with synthetic Tauri/storage adapters only.
 * Observation never replays an old mutation. Each fresh user intent performs
 * one native content/cache check; exact USB submission precedes persistence.
 */
import test from "node:test";
import assert from "node:assert/strict";
import { AGENT_APPEARANCE_MAP_STORAGE_KEY, ENABLED_AGENTS_STORAGE_KEY } from "./agent-appearance-config.js";
import {
  ACTIVE_APPEARANCE_KEY,
  APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE,
  APPEARANCE_CONFIRMATION_REQUIRED_MESSAGE,
  APPEARANCE_SYNC_CANCELLED_MESSAGE,
  CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE,
  applyDesktopPetAssignment,
  reconcileDesktopPetAppearance,
} from "./desktop-pet-assignment.js";

const BOARD = "fixture-board";
const FOLLOW = "00000000-0000-4000-8000-000000000001";
const USB = { connected: true, boardDeviceId: BOARD, connectionId: "fixture-connection", bootId: "fixture-boot" };
const PROOF = {
  status: "observed_current_state_quiescent", oldResult: "unknown",
  mutationSent: false, requiresFreshUserIntent: true,
  activeSlot: 1, activePackId: "fixture-existing-pack",
};
const PROFILE = {
  desktopDeviceId: "fixture-desktop", mqttUrl: "mqtt://example.invalid",
  mqttNamespace: "fixture", mqttUsername: "", mqttPassword: "",
  transport: "mqtt", serialPort: "", serialBaud: 115200, petChannelId: "fixture",
  enabledAgents: ["codex"], selectedAgentId: "codex",
};

function fixture(t, options = {}) {
  const calls = [], storage = new Map();
  const previous = Object.getOwnPropertyDescriptor(globalThis, "localStorage");
  const memoryStorage = {
    getItem: key => storage.get(key) ?? null,
    setItem: (key, value) => storage.set(key, String(value)),
    removeItem: key => storage.delete(key),
  };
  Object.defineProperty(globalThis, "localStorage", { configurable: true, value: memoryStorage });
  t.after(() => {
    if (previous) Object.defineProperty(globalThis, "localStorage", previous);
    else delete globalThis.localStorage;
  });
  let statusIndex = 0;
  const respond = (value, args) => {
    if (value instanceof Error) throw value;
    return typeof value === "function" ? value(args) : value;
  };
  const invoke = async (command, args) => {
    calls.push({ command, args });
    switch (command) {
      case "usb_get_status":
        return respond(options.statuses
          ? options.statuses[Math.min(statusIndex++, options.statuses.length - 1)] : { ...USB }, args);
      case "usb_reconcile_appearance_unknown":
        assert.equal(args.expectedBoardDeviceId, BOARD);
        assert.match(args.userIntentId, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i);
        assert.notEqual(args.userIntentId, FOLLOW);
        if (!Object.hasOwn(options, "observe")) throw new Error("appearance_recovery_no_pending");
        return respond(options.observe, args);
      case "load_bridge_profile":
        return { ...PROFILE, ...options.profile };
      case "usb_sync_appearance":
        return respond(options.sync ?? { ok: true, fileCount: 4, byteCount: 4096 }, args);
      case "dispatch_remote_cli_binding":
        return respond(options.dispatch ?? {
          ok: true, usbSent: true, boardDeviceId: args.input.boardDeviceId,
          targetDeviceId: args.input.targetDeviceId, targetSource: args.input.targetSource,
        }, args);
      case "save_bridge_profile":
        return respond(options.save ?? {}, args);
      case "ensure_bridge_runtime":
        return respond(options.bridge ?? { running: true }, args);
      default:
        throw new Error("Unexpected mock command: " + command);
    }
  };
  return {
    calls, storage, invoke,
    count: command => calls.filter(x => x.command === command).length,
    commands: () => calls.map(x => x.command),
    run: overrides => applyDesktopPetAssignment({
      invoke, listen: null, agentAppearanceMap: { codex: "old-avatar" },
      agentId: "claude-code", appearance: { id: "new-avatar", name: "新形象" },
      agentOptions: [{ id: "claude-code", label: "Claude Code" }, { id: "codex", label: "Codex" }],
      boardDeviceId: BOARD, currentAppearanceId: "old-avatar",
      followIntentId: FOLLOW, ...overrides,
    }),
  };
}
function noCommit(f) {
  for (const command of ["dispatch_remote_cli_binding", "save_bridge_profile", "ensure_bridge_runtime"]) {
    assert.equal(f.count(command), 0, command + " must not run");
  }
  assert.equal(f.storage.size, 0, "unconfirmed operations must not persist local state");
}
function confirmationRequired(error) {
  assert.equal(error.code, "appearance-confirmation-required");
  assert.equal(error.message, APPEARANCE_CONFIRMATION_REQUIRED_MESSAGE);
  return true;
}

test("no pending observation precedes one fresh sync, exact dispatch, then one profile commit", async t => {
  const f = fixture(t);
  const result = await f.run();
  assert.deepEqual(f.commands(), [
    "usb_get_status", "usb_reconcile_appearance_unknown", "usb_get_status",
    "load_bridge_profile", "usb_sync_appearance", "dispatch_remote_cli_binding",
    "save_bridge_profile", "ensure_bridge_runtime",
  ]);
  assert.deepEqual(f.calls.find(x => x.command === "usb_sync_appearance").args,
    { appearanceId: "new-avatar", boardDeviceId: BOARD });
  assert.deepEqual(f.calls.find(x => x.command === "dispatch_remote_cli_binding").args.input, {
    boardDeviceId: BOARD, targetDeviceId: PROFILE.desktopDeviceId,
    expectedConnectionId: USB.connectionId, expectedBootId: USB.bootId,
    targetSource: "claude-code", previousSource: "codex", mqttNamespace: "fixture",
  });
  const saved = f.calls.find(x => x.command === "save_bridge_profile").args.input;
  assert.deepEqual(saved.enabledAgents, ["claude-code"]);
  assert.equal(saved.selectedAgentId, "claude-code");
  assert.deepEqual(result.nextMap, { codex: "old-avatar", "claude-code": "new-avatar" });
  assert.deepEqual(JSON.parse(f.storage.get(AGENT_APPEARANCE_MAP_STORAGE_KEY)), result.nextMap);
  assert.deepEqual(JSON.parse(f.storage.get(ENABLED_AGENTS_STORAGE_KEY)), ["claude-code"]);
  assert.equal(f.storage.get(ACTIVE_APPEARANCE_KEY), "new-avatar");
});

test("exact quiescent proof permits only this new intent's single ordinary sync", async t => {
  const f = fixture(t, { observe: { ...PROOF } });
  await f.run();
  assert.equal(f.count("usb_reconcile_appearance_unknown"), 1);
  assert.equal(f.count("usb_sync_appearance"), 1);
  assert.equal(f.count("dispatch_remote_cli_binding"), 1);
  assert.ok(f.commands().indexOf("usb_reconcile_appearance_unknown") < f.commands().indexOf("usb_sync_appearance"));
  assert.deepEqual(Object.keys(f.calls.find(x => x.command === "usb_sync_appearance").args).sort(),
    ["appearanceId", "boardDeviceId"]);
});

test("read-only reconciliation distinguishes no pending from an observed old unknown", async t => {
  const f = fixture(t);
  assert.deepEqual(await reconcileDesktopPetAppearance({ invoke: f.invoke, boardDeviceId: BOARD }),
    { observed: false, noPending: true, message: "设备已就绪。" });
  assert.deepEqual(f.commands(), ["usb_get_status", "usb_reconcile_appearance_unknown"]);
  noCommit(f);
});

test("same local appearance ID still asks native to confirm its content/cache once", async t => {
  const f = fixture(t, { sync: { ok: true, reusedSlot: true, fileCount: 0, byteCount: 0 } });
  const result = await f.run({
    agentId: "codex", appearance: { id: "same-avatar", name: "同一形象" },
    agentAppearanceMap: { codex: "same-avatar" }, currentAppearanceId: "same-avatar",
  });
  assert.equal(f.count("usb_reconcile_appearance_unknown"), 1);
  assert.equal(f.count("usb_sync_appearance"), 1);
  assert.equal(f.count("dispatch_remote_cli_binding"), 1);
  assert.equal(f.count("save_bridge_profile"), 0);
  assert.match(result.notice, /设备缓存切换/);
  assert.match(result.notice, /无需重新传输素材/);
});

test("same appearance with a different follow channel uses native cache confirmation before saving", async t => {
  const f = fixture(t, { sync: { ok: true, reusedSlot: true, fileCount: 0, byteCount: 0 } });
  await f.run({ appearance: { id: "same-avatar", name: "同一形象" }, currentAppearanceId: "same-avatar" });
  assert.equal(f.count("usb_sync_appearance"), 1);
  assert.equal(f.count("dispatch_remote_cli_binding"), 1);
  assert.equal(f.count("save_bridge_profile"), 1);
  assert.equal(f.storage.get(ACTIVE_APPEARANCE_KEY), "same-avatar");
});

for (const [name, overrides, message] of [
  ["unchanged appearance follow", { currentAppearanceId: "new-avatar" }, CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE],
  ["changed appearance", {}, APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE],
]) {
  test("offline " + name + " refuses before observation or persistence", async t => {
    const f = fixture(t, { statuses: [{ connected: false }] });
    await assert.rejects(() => f.run(overrides), { message });
    assert.deepEqual(f.commands(), ["usb_get_status"]);
    noCommit(f);
  });
}

test("numeric progress retains the read-only confirmation step and removes its listener", async t => {
  const f = fixture(t), progress = [];
  let removed = 0;
  await f.run({
    onProgress: value => progress.push(value),
    listen: async (event, callback) => {
      assert.equal(event, "usb-sync-progress");
      callback({ payload: { currentFile: 2, totalFiles: 4, bytesSent: 2048, bytesTotal: 4096 } });
      return () => { removed++; };
    },
  });
  assert.deepEqual(progress, [
    { stage: "confirming", text: "正在确认…", indeterminate: true, followOnly: true },
    { type: "info", text: "USB 传输中… 2/4 个素材 (2.0 KB/4.0 KB) 50%",
      currentFile: 2, totalFiles: 4, bytesSent: 2048, bytesTotal: 4096, percent: 50 },
  ]);
  assert.equal(removed, 1);
});

test("named storage preparation follows confirmation without inventing byte progress", async t => {
  const f = fixture(t), progress = [];
  await f.run({
    onProgress: value => progress.push(value),
    listen: async (_event, callback) => {
      callback({ payload: { stage: "preparing_storage" } });
      callback({ payload: { stage: "unknown_future_stage" } });
      return () => {};
    },
  });
  assert.deepEqual(progress, [
    { stage: "confirming", text: "正在确认…", indeterminate: true, followOnly: true },
    { type: "info", stage: "preparing_storage",
      text: "设备正在擦除旧素材、准备存储空间…", percent: 0, indeterminate: true },
  ]);
});

test("new unknown sync is not retried or followed by another observation/dispatch/save", async t => {
  const f = fixture(t, { observe: { ...PROOF }, sync: new Error("appearance_result_unknown_read_only_no_replay") });
  await assert.rejects(() => f.run(), confirmationRequired);
  assert.equal(f.count("usb_reconcile_appearance_unknown"), 1);
  assert.equal(f.count("usb_sync_appearance"), 1);
  noCommit(f);
});

test("explicit asset cancellation is preserved after one observation and one sync", async t => {
  const f = fixture(t, { sync: { ok: false, error: APPEARANCE_SYNC_CANCELLED_MESSAGE } });
  await assert.rejects(() => f.run(), { message: APPEARANCE_SYNC_CANCELLED_MESSAGE });
  assert.equal(f.count("usb_reconcile_appearance_unknown"), 1);
  assert.equal(f.count("usb_sync_appearance"), 1);
  noCommit(f);
});

for (const code of [
  "appearance_recovery_busy",
  "appearance_recovery_not_quiescent_or_unproven",
  "appearance_recovery_requires_firmware_update",
  "appearance_raw_stream_quarantined_no_probe",
  "appearance_recovery_connection_changed",
  "unclassified_observation_failure",
]) {
  test("observation failure " + code + " never attempts a mutation or profile read", async t => {
    const f = fixture(t, { observe: new Error(code) });
    await assert.rejects(() => f.run(), error => {
      assert.doesNotMatch(error.message, /appearance_/);
      return true;
    });
    const observations = f.calls.filter(x => x.command === "usb_reconcile_appearance_unknown");
    assert.ok(code === "appearance_recovery_busy" ? observations.length > 1 && observations.length <= 24 : observations.length === 1);
    assert.equal(new Set(observations.map(x => x.args.userIntentId)).size, observations.length);
    assert.equal(f.count("usb_sync_appearance"), 0);
    assert.equal(f.count("load_bridge_profile"), 0);
    noCommit(f);
  });
}

for (const [name, delta] of [
  ["wrong status", { status: "observed_old_ack" }],
  ["known old result", { oldResult: "success" }],
  ["mutation was sent", { mutationSent: true }],
  ["no fresh user intent required", { requiresFreshUserIntent: false }],
  ["invalid active slot", { activeSlot: 2 }],
  ["empty pack identity", { activePackId: "" }],
  ["invalid pack identity", { activePackId: "bad\nidentity" }],
]) {
  test("inexact observation proof (" + name + ") cannot authorize a new sync", async t => {
    const f = fixture(t, { observe: { ...PROOF, ...delta } });
    await assert.rejects(() => f.run(), confirmationRequired);
    assert.equal(f.count("usb_reconcile_appearance_unknown"), 1);
    assert.equal(f.count("usb_sync_appearance"), 0);
    noCommit(f);
  });
}

for (const [name, delta] of [
  ["board", { boardDeviceId: "different-fixture-board" }],
  ["connection", { connectionId: "different-fixture-connection" }],
  ["boot", { bootId: "different-fixture-boot" }],
  ["disconnect", { connected: false }],
]) {
  test("changed " + name + " after observation stops before loading profile or mutating", async t => {
    const f = fixture(t, { observe: { ...PROOF }, statuses: [{ ...USB }, { ...USB, ...delta }] });
    await assert.rejects(() => f.run(), confirmationRequired);
    assert.equal(f.count("load_bridge_profile"), 0);
    assert.equal(f.count("usb_sync_appearance"), 0);
    noCommit(f);
  });
}

for (const [name, delta] of [
  ["ok", { ok: false }], ["USB submission", { usbSent: false }],
  ["agent", { targetSource: "different-fixture-agent" }],
  ["desktop", { targetDeviceId: "different-fixture-desktop" }],
  ["board", { boardDeviceId: "different-fixture-board" }],
]) {
  test("mismatched dispatch " + name + " never persists or retries a device operation", async t => {
    const f = fixture(t, { dispatch: args => ({
      ok: true, usbSent: true, ...args.input, ...delta,
    }) });
    await assert.rejects(() => f.run(), { message: "本次切换结果尚未确认，请检查连接后再试。" });
    assert.equal(f.count("usb_sync_appearance"), 1);
    assert.equal(f.count("dispatch_remote_cli_binding"), 1);
    assert.equal(f.count("save_bridge_profile"), 0);
    assert.equal(f.count("ensure_bridge_runtime"), 0);
    assert.equal(f.storage.size, 0);
  });
}

test("intent invalidated by a late observation cannot start the fresh mutation", async t => {
  let current = true;
  const f = fixture(t, { observe: () => { current = false; return { ...PROOF }; } });
  await assert.rejects(() => f.run({ isCurrent: () => current }), confirmationRequired);
  assert.equal(f.count("usb_sync_appearance"), 0);
  noCommit(f);
});

test("intent invalidated after asset confirmation cannot dispatch or save", async t => {
  let current = true;
  const f = fixture(t, { sync: () => { current = false; return { ok: true }; } });
  await assert.rejects(() => f.run({ isCurrent: () => current }), confirmationRequired);
  assert.equal(f.count("usb_sync_appearance"), 1);
  noCommit(f);
});

test("profile-save failure after exact USB submission cannot replay or locally commit", async t => {
  const f = fixture(t, { save: new Error("fixture save failed") });
  await assert.rejects(() => f.run(), /fixture save failed/);
  assert.equal(f.count("usb_sync_appearance"), 1);
  assert.equal(f.count("dispatch_remote_cli_binding"), 1);
  assert.equal(f.count("save_bridge_profile"), 1);
  assert.equal(f.count("ensure_bridge_runtime"), 0);
  assert.equal(f.storage.size, 0);
});

test("bridge restart failure reports the committed assignment rather than replaying it", async t => {
  const f = fixture(t, { bridge: new Error("fixture bridge unavailable") });
  await assert.rejects(() => f.run(), error => {
    assert.equal(error.committedAssignment.agentId, "claude-code");
    assert.deepEqual(error.committedAssignment.enabledAgents, ["claude-code"]);
    return true;
  });
  assert.equal(f.count("usb_reconcile_appearance_unknown"), 1);
  assert.equal(f.count("usb_sync_appearance"), 1);
  assert.equal(f.count("dispatch_remote_cli_binding"), 1);
  assert.equal(f.count("save_bridge_profile"), 1);
  assert.equal(f.count("ensure_bridge_runtime"), 1);
  assert.equal(f.storage.get(ACTIVE_APPEARANCE_KEY), "new-avatar");
});


test("follow carries the pre-transfer connection and never adopts a reconnected native generation", async t => {
  let nativeGeneration = USB.connectionId, received;
  const f = fixture(t, {
    sync: () => { nativeGeneration = "replacement-connection"; return { ok: true, fileCount: 1, byteCount: 1 }; },
    dispatch: ({ input }) => {
      received = input;
      throw new Error("follow USB scope changed; not submitted");
    },
  });
  await assert.rejects(() => f.run(), /未确认/);
  assert.equal(f.count("dispatch_remote_cli_binding"), 1);
  assert.equal(f.count("usb_sync_appearance"), 1);
  assert.equal(received.expectedConnectionId, USB.connectionId);
  assert.equal(received.expectedBootId, USB.bootId);
  assert.notEqual(received.expectedConnectionId, nativeGeneration);
  assert.equal(f.count("save_bridge_profile"), 0);
  assert.equal(f.count("ensure_bridge_runtime"), 0);
  assert.equal(f.storage.size, 0);
});

for (const [prefix,outcome] of [["APPEARANCE_RESULT_UNKNOWN:","unknown"],["APPEARANCE_NOT_SUBMITTED:","not_submitted"],["untyped failure", "unknown"]]) {
 test("activation outcome " + prefix + " is accurate and never retries", async t => {
  const f=fixture(t,{sync:{ok:false,error:prefix+" private-path-fixture-not-for-display"}});
  await assert.rejects(()=>f.run(),e=>{
   if(outcome==="unknown") assert.equal(e.message.includes("本次未切换"),false);
   assert.equal(e.outcome,outcome);
   assert.equal(e.message.includes("private-path-fixture"),false);
   if(outcome==="unknown") {assert.equal(e.message.includes("本次未切换"),false);assert.equal(e.message.includes("结果"),true);}
   else assert.match(e.message,/尚未提交/);
   return true;
  });
  assert.equal(f.count("usb_sync_appearance"),1);assert.equal(f.count("usb_reconcile_appearance_unknown"),1);noCommit(f);
 });
}
test("persisted unknown cancellation remains unknown rather than asserting stopped",async t=>{
 const f=fixture(t,{sync:{ok:false,error:"APPEARANCE_RESULT_UNKNOWN: "+APPEARANCE_SYNC_CANCELLED_MESSAGE}});
 await assert.rejects(()=>f.run(),e=>e.outcome==="unknown" && e.message!==APPEARANCE_SYNC_CANCELLED_MESSAGE);noCommit(f);assert.equal(f.count("usb_sync_appearance"),1);
});


test("video capacity refusal is explicit and cannot commit a follow switch",async t=>{
 const f=fixture(t,{sync:{ok:false,error:"APPEARANCE_NOT_SUBMITTED: P4 appearance videos require 17972330 bytes but the custom slot only has 10481664 bytes"}});
 await assert.rejects(f.run(),e=>e.code==='appearance-capacity-exceeded'&&e.outcome==='not_submitted'&&e.message.includes('可用空间'));
 assert.equal(f.count('usb_sync_appearance'),1);noCommit(f);
});
