"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const { syncWorkBuddyHooks, EVENTS } = require("../hooks/workbuddy-install");
const { workbuddyPaths } = require("../hooks/workbuddy-paths");
const { stateForHook } = require("../hooks/workbuddy-pet-manager-hook");
const { displayText, readCurrentReply } = require("../hooks/workbuddy-display");
const { createAgentSessionBus, WorkBuddyAdapter } = require("../packages/agent-session-bus/src");
const { publishClawdState } = require("../packages/clawd-backend-service/src/headless-mqtt");

test("WorkBuddy lifecycle reaches the device with bounded current-turn prose only", () => {
  for (const event of EVENTS) {
    const payload = stateForHook({ hook_event_name: event, session_id: "s", prompt: "PRIVATE_SENTINEL" });
    let received;
    publishClawdState({ publisher: { publishSource(value) { received = value; } } }, payload);
    assert.equal(received.source, "workbuddy");
    assert.equal(received.sessionId, "s");
    assert.ok(received.display.title);
    assert.ok(received.display.content);
    assert.equal(received.display.status, received.state);
    if (event === "UserPromptSubmit") assert.equal(received.display.content, "PRIVATE_SENTINEL");
    else assert.doesNotMatch(JSON.stringify(received), /PRIVATE_SENTINEL/);
  }
});

function fixture(t) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "pet-workbuddy-test-"));
  t.after(() => fs.rmSync(home, { recursive: true, force: true }));
  const options = { home, env: {}, platform: "linux" };
  return { home, options, ...workbuddyPaths(options) };
}
test("WorkBuddy config remains isolated from CodeBuddy and Windows paths are discovered", () => {
  assert.equal(workbuddyPaths({ home: "/fixture", env: { CODEBUDDY_CONFIG_DIR: "/other" } }).root, path.join("/fixture", ".workbuddy"));
  assert.equal(workbuddyPaths({ home: "/fixture", env: { WORKBUDDY_CONFIG_DIR: "/custom" } }).root, "/custom");
  const win = workbuddyPaths({ home: "/fixture", platform: "win32", env: { LOCALAPPDATA: "/local", ProgramFiles: "/programs" } });
  assert.deepEqual(win.apps, [
    path.join("/local", "Programs", "WorkBuddy", "WorkBuddy.exe"),
    path.join("/programs", "WorkBuddy", "WorkBuddy.exe"),
  ]);
});
test("WorkBuddy hooks preserve user config, back up, update paths and are idempotent", t => {
  const f = fixture(t); fs.mkdirSync(f.root);
  const original = JSON.stringify({ unrelated: { custom: true }, hooks: { Stop: [{ hooks: [{ type: "command", command: "user-hook" }] }] } });
  fs.writeFileSync(f.settings, original);
  assert.equal(syncWorkBuddyHooks(f.options).updated, true);
  assert.equal(fs.readFileSync(`${f.settings}.pet-manager-backup`, "utf8"), original);
  const installed = fs.readFileSync(f.settings, "utf8");
  const config = JSON.parse(installed);
  assert.equal(config.unrelated.custom, true);
  assert.equal(config.hooks.Stop[0].hooks[0].command, "user-hook");
  for (const event of EVENTS) assert.ok(config.hooks[event].some(e => e.hooks.some(h => h.command.includes("workbuddy-pet-manager-hook.js"))));
  assert.equal(syncWorkBuddyHooks(f.options).updated, false);
  assert.equal(fs.readFileSync(f.settings, "utf8"), installed);
  syncWorkBuddyHooks({ ...f.options, nodeBin: "/new path/node" });
  const updated = JSON.parse(fs.readFileSync(f.settings, "utf8"));
  assert.equal(updated.hooks.Stop.length, 2);
  assert.match(updated.hooks.Stop[1].hooks[0].command, /new path\/node/);
  assert.equal(fs.readFileSync(`${f.settings}.pet-manager-backup`, "utf8"), original);
});
test("missing, invalid and explicitly disabled WorkBuddy config is not overwritten", t => {
  const f = fixture(t);
  assert.equal(syncWorkBuddyHooks(f.options).skipped, true);
  assert.equal(fs.existsSync(f.root), false);
  fs.mkdirSync(f.root);
  for (const bad of ["{", "[]", '{"hooks":{"Stop":{}}}']) {
    fs.writeFileSync(f.settings, bad);
    assert.throws(() => syncWorkBuddyHooks(f.options));
    assert.equal(fs.readFileSync(f.settings, "utf8"), bad);
  }
  const disabled = '{"disableAllHooks":true}';
  fs.writeFileSync(f.settings, disabled);
  assert.equal(syncWorkBuddyHooks(f.options).skipped, true);
  assert.equal(fs.readFileSync(f.settings, "utf8"), disabled);
});
test("WorkBuddy lifecycle rejects raw secrets, paths and permission decisions", () => {
  for (const [event, state] of Object.entries({ UserPromptSubmit: "working", PreToolUse: "working", PermissionRequest: "waiting_user", Stop: "done", StopFailure: "error" })) {
    const result = stateForHook({ hook_event_name: event, session_id: "session-a", prompt: "用户的问题", api_key: "secret", transcript_path: "/private", tool_input: { command: "secret" } });
    assert.equal(result.state, state); assert.equal(result.agent_id, "workbuddy");
    assert.doesNotMatch(JSON.stringify(result), /secret|private|decision|permissionDecision/);
  }
  assert.equal(stateForHook({ hook_event_name: "SubagentStop", session_id: "a" }), null);
  assert.equal(stateForHook({ hook_event_name: "Stop" }), null);
  assert.equal(stateForHook({ hook_event_name: "__proto__", session_id: "a" }), null);
  const result = spawnSync(process.execPath, [path.join(__dirname, "../hooks/workbuddy-pet-manager-hook.js")], { input: "invalid json", encoding: "utf8", timeout: 2000 });
  assert.equal(result.status, 0); assert.equal(result.stdout.trim(), "{}");
});

test("WorkBuddy completed answer reaches the exact-session device display", () => {
  const payload = stateForHook({ hook_event_name: "Stop", session_id: "session-a",
    last_assistant_message: "北京今天晴，出门记得带水。", tool_input: { api_key: "DO_NOT_FORWARD" } });
  let received;
  publishClawdState({ publisher: { publishSource(value) { received = value; } } }, payload);
  assert.equal(received.sessionId, "session-a");
  assert.equal(received.display.content, "北京今天晴，出门记得带水。");
  assert.equal(received.state, "done");
  assert.doesNotMatch(JSON.stringify(received), /DO_NOT_FORWARD/);
});

test("WorkBuddy display strips reasoning, code, known credentials and local paths", () => {
  const text = displayText('<think>HIDDEN_THOUGHT</think>回复🙂 ```KEY_CODE``` sk-abcdefghijklmnop {"api_key":"HIDDEN_KEY"} Bearer HIDDEN_TOKEN /Users/example/private.txt');
  assert.match(text, /回复🙂/);
  assert.doesNotMatch(text, /HIDDEN_|KEY_CODE|abcdefghijklmnop|\/Users/);
  assert.equal(Array.from(displayText("🙂".repeat(1000))).length, 320);
});

test("WorkBuddy transcript fallback is session, turn, path and role scoped", t => {
  const f = fixture(t); const dir = path.join(f.root, "projects", "example");
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, "session-a.jsonl");
  const user = { type: "message", role: "user", sessionId: "session-a", content: [{ type: "input_text", text: "QUESTION" }] };
  const answer = { type: "message", role: "assistant", status: "completed", sessionId: "session-a",
    providerData: { conversationRequestId: "turn-a" }, content: [{ type: "output_text", text: "这就是本轮回答。" }, { type: "thinking", text: "HIDDEN" }] };
  const payload = { hook_event_name: "Stop", session_id: "session-a", generation_id: "turn-a", transcript_path: file };
  fs.writeFileSync(file, [user, answer].map(JSON.stringify).join("\n") + "\n");
  assert.equal(readCurrentReply(payload, f.options), "这就是本轮回答。");
  assert.equal(stateForHook(payload, f.options).display_content, "这就是本轮回答。");
  assert.equal(readCurrentReply({ ...payload, generation_id: "turn-b" }, f.options), "");
  assert.equal(readCurrentReply({ ...payload, session_id: "other" }, f.options), "");
  fs.appendFileSync(file, JSON.stringify(user) + "\n");
  assert.equal(readCurrentReply(payload, f.options), "");
  const outside = path.join(f.home, "session-a.jsonl");
  fs.writeFileSync(outside, JSON.stringify(answer));
  assert.equal(readCurrentReply({ ...payload, transcript_path: outside }, f.options), "");
  fs.unlinkSync(file); fs.symlinkSync(outside, file);
  assert.equal(readCurrentReply(payload, f.options), "");
});

test("WorkBuddy transcript tail is bounded and ignores malformed/tool records", t => {
  const f = fixture(t); const dir = path.join(f.root, "projects"); fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, "session-a.jsonl");
  fs.writeFileSync(file, "x".repeat(300000) + "\n" + JSON.stringify({ type: "function_call_result", sessionId: "session-a", output: "PRIVATE_TOOL_RESULT" }) + "\n");
  const payload = { session_id: "session-a", transcript_path: file };
  assert.equal(readCurrentReply(payload, f.options), "");
  fs.appendFileSync(file, JSON.stringify({ type: "message", role: "assistant", status: "completed", sessionId: "session-a", content: [{ type: "output_text", text: "新回复" }] }) + "\n");
  assert.equal(readCurrentReply(payload, f.options), "新回复");
});
test("WorkBuddy hook sessions enter the bus without reading private databases or injecting text", async t => {
  const f = fixture(t); fs.mkdirSync(f.root);
  const adapter = new WorkBuddyAdapter(f.options);
  assert.equal((await adapter.isAvailable()).ready, true);
  const bus = createAgentSessionBus({ port: 0, adapters: [adapter], log: () => {}, sessionStatusProvider: id => id === "workbuddy" ? [
    { id: "workbuddy:session-a", state: "working", updatedAt: 20 },
    { id: "workbuddy:session-b", state: "done", updatedAt: 10 },
  ] : [] });
  const port = await bus.start();
  try {
    const data = await (await fetch(`http://127.0.0.1:${port}/agent/sessions?agentId=workbuddy`)).json();
    assert.deepEqual(data.sessions.map(s => s.id), ["session-a", "session-b"]);
    assert.equal(data.sessions[0].state, "working");
    assert.equal(data.sessions[0].name, "WorkBuddy 会话");
    const events = []; for await (const event of adapter.inject({ text: "hello" })) events.push(event);
    assert.equal(events[0].code, "AGENT_INPUT_UNSUPPORTED");
    await assert.rejects(() => adapter.openNew(), /WorkBuddy/);
  } finally { await bus.stop(); }
});
