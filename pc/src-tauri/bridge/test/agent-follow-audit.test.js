"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const {
  publishClawdState, publishPermissionRequest, OpenClawStatusController,
  resolvePermissionDecision, permissionHookResponse,
} = require("../packages/clawd-backend-service/src/headless-mqtt");

test("Claude permission channel has a bubble without exposing tool arguments", () => {
  let out;
  publishPermissionRequest({ publisher: { publishSource(p) { out = p; } } }, {
    session_id: "fixture", tool_name: "Bash", tool_input: { command: "PRIVATE_SENTINEL" },
  });
  assert.equal(out.state, "waiting_user");
  assert.equal(out.sessionId, "fixture");
  assert.equal(out.display.content, "等待确认");
  assert.doesNotMatch(JSON.stringify(out), /PRIVATE_SENTINEL/);
});

test("Claude and MiMoCode active states never lose their device display", () => {
  for (const agent_id of ["claude-code", "mimocode"]) {
    for (const [state, event] of [["working", "PreToolUse"], ["speaking", "AssistantOutput"], ["waiting_user", "PermissionRequest"], ["done", "Stop"], ["error", "StopFailure"]]) {
      let out;
      publishClawdState({ publisher: { publishSource(p) { out = p; } } }, { agent_id, session_id: "fixture", state, event });
      assert.equal(out.source, agent_id);
      assert.equal(out.sessionId, "fixture");
      assert.ok(out.display?.content, `${agent_id} ${state} missing display`);
      assert.equal(out.display.status, state);
    }
  }
});

test("OpenClaw lifecycle publishes bubbles but does not revive them on heartbeat or disconnect", () => {
  const rows = [];
  const controller = new OpenClawStatusController({ publisher: { publishSource(p) { rows.push(p); }, publishAvailability() {} } });
  controller.publishCurrent(true, { silent: true });
  assert.equal(rows.at(-1).display, undefined);
  for (const state of ["delta", "final", "aborted", "error"]) {
    controller.consumeEvent("chat", { state, sessionKey: "fixture", runId: "run", message: "PRIVATE_SENTINEL" });
    assert.ok(rows.at(-1).display.content);
    assert.doesNotMatch(JSON.stringify(rows.at(-1)), /PRIVATE_SENTINEL/);
  }
  const timestamp = rows.at(-1).display.updatedAtMs;
  controller.publishCurrent(true, { silent: true });
  assert.equal(rows.at(-1).display.updatedAtMs, timestamp);
  controller.gatewayConnected = true;
  controller.setGatewayConnected(false);
  assert.equal(rows.at(-1).display, undefined);
});

test("status observation never approves Claude permission requests by default", () => {
  for (const permissionBehavior of [undefined, "passthrough", "invalid"]) {
    assert.deepEqual(permissionHookResponse(resolvePermissionDecision({ permissionBehavior }, {})), {});
  }
  assert.deepEqual(permissionHookResponse({ allow: true }), {});
  for (const permissionBehavior of ["allow", "deny"]) {
    assert.equal(permissionHookResponse(resolvePermissionDecision({ permissionBehavior }, {})).hookSpecificOutput.decision.behavior, permissionBehavior);
  }
});
