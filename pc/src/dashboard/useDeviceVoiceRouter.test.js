/**
 * [Input] Device voice transcript/progress/delivery state-machine actions.
 * [Output] Behavioral coverage for frozen/current-fallback routes, draft-ready confirmation, monotonic revisions, stale utterances, and one terminal delivery.
 * [Pos] Test node for the dashboard device-voice router.
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  DEVICE_VOICE_ROUTER_INITIAL_STATE,
  deviceVoiceRouterReducer,
  transcriptMessage,
  voiceFlowMessage,
} from "./useDeviceVoiceRouter.js";

test("Failed visible composer delivery keeps the actual cause beside its summary", () => {
  for (const composerMode of ["visible", "focused-input", ""]) {
    const state = deviceVoiceRouterReducer(DEVICE_VOICE_ROUTER_INITIAL_STATE, {
      type: "delivery", ok: false, composerMode,
      message: "WorkBuddy 前台会话未定位，语音草稿未写入",
      composerError: "WorkBuddy 输入框未就绪（节点=12，可读=0，可写=0）",
    });
    assert.match(state.flow.composerError, /节点=12/);
    assert.match(voiceFlowMessage(state.flow), /前台会话未定位.*\n具体原因：.*节点=12/s);
  }
});

test("Voice failure details are not duplicated or displayed after success", () => {
  assert.equal(voiceFlowMessage({ phase: "error", message: "失败：原因", composerError: "原因" }), "失败：原因");
  assert.equal(voiceFlowMessage({ phase: "done", ok: true, message: "已发送", composerError: "旧错误" }), "已发送");
  assert.equal(voiceFlowMessage({ phase: "error", composerError: "原因" }), "具体原因：原因");
  assert.equal(voiceFlowMessage({ phase: "error", message: "发送失败" }), "发送失败");
  assert.equal(voiceFlowMessage(null), "");
  const state = deviceVoiceRouterReducer({
    ...DEVICE_VOICE_ROUTER_INITIAL_STATE,
    flow: { ...DEVICE_VOICE_ROUTER_INITIAL_STATE.flow, composerError: "旧错误" },
  }, { type: "delivery", ok: true, composerMode: "visible" });
  assert.equal(state.flow.composerError, "");
});

test("WorkBuddy describes final-only append without promising streaming editor writes", () => {
  assert.match(transcriptMessage({ agentId: "workbuddy" }, "listening", "visible"), /松开后.*一次追加/);
  assert.match(transcriptMessage({ agentId: "workbuddy" }, "partial", "visible"), /松开后一次写入/);
  assert.match(transcriptMessage({ agentId: "workbuddy" }, "draft_ready", "visible"), /已追加到 WorkBuddy/);
  assert.match(transcriptMessage({ agentId: "codex" }, "partial", "visible"), /实时识别并同步/);
});

function transcript(state, value) {
  return deviceVoiceRouterReducer(state, {
    type: "transcript",
    nowMs: 1000,
    ok: true,
    ...value,
  });
}

test("device voice route freezes on listening while revisions and phases stay monotonic", () => {
  const listening = transcript(DEVICE_VOICE_ROUTER_INITIAL_STATE, {
    utteranceId: "utterance-a",
    phase: "listening",
    revision: 0,
    agentId: "codex",
    sessionId: "session-a",
  });
  const partial = transcript(listening, {
    utteranceId: "utterance-a",
    phase: "partial",
    revision: 2,
    text: "hello",
    agentId: "claude-code",
    sessionId: "session-b",
  });

  assert.equal(partial.flow.phase, "partial");
  assert.equal(partial.flow.agentId, "codex");
  assert.equal(partial.flow.sessionId, "session-a");
  assert.strictEqual(transcript(partial, {
    utteranceId: "utterance-a",
    phase: "listening",
    revision: 1,
  }), partial);

  const draftReady = transcript(partial, {
    utteranceId: "utterance-a",
    phase: "draft_ready",
    revision: 3,
    isFinal: true,
    text: "final text",
    message: "已追加到输入框",
  });
  assert.equal(draftReady.flow.phase, "draft_ready");

  const delayedPartial = transcript(draftReady, {
    utteranceId: "utterance-a",
    phase: "partial",
    revision: 4,
    text: "stale partial",
    message: "正在识别",
  });
  assert.strictEqual(delayedPartial, draftReady);
  assert.equal(delayedPartial.flow.phase, "draft_ready");
  assert.equal(delayedPartial.flow.text, "final text");
  assert.equal(delayedPartial.flow.isFinal, true);
  assert.equal(delayedPartial.flow.message, "已追加到输入框");

  const submitting = transcript(delayedPartial, {
    utteranceId: "utterance-a",
    phase: "submitting",
    revision: 4,
  });
  assert.equal(submitting.flow.phase, "submitting");
});

test("a new listening utterance retires the old id and rejects delayed old results", () => {
  const first = transcript(DEVICE_VOICE_ROUTER_INITIAL_STATE, {
    utteranceId: "utterance-a",
    phase: "listening",
    agentId: "codex",
    sessionId: "session-a",
  });
  const second = transcript(first, {
    utteranceId: "utterance-b",
    phase: "listening",
    agentId: "codex",
    sessionId: "session-b",
  });
  assert.deepEqual(second.retiredUtteranceIds, ["utterance-a"]);

  const delayed = deviceVoiceRouterReducer(second, {
    type: "delivery",
    utteranceId: "utterance-a",
    ok: true,
    text: "old",
    message: "old result",
  });
  assert.strictEqual(delayed, second);
  assert.equal(delayed.flow.sessionId, "session-b");
});

test("one utterance accepts only one terminal delivery while auto may resolve once", () => {
  const listening = transcript(DEVICE_VOICE_ROUTER_INITIAL_STATE, {
    utteranceId: "utterance-a",
    phase: "listening",
    agentId: "openclaw",
    sessionId: "auto",
  });
  const pending = deviceVoiceRouterReducer(listening, {
    type: "delivery",
    utteranceId: "utterance-a",
    pending: true,
    ok: true,
    sessionId: "session-resolved",
    message: "waiting",
  });
  assert.equal(pending.flow.phase, "waiting_reply");
  assert.equal(pending.flow.sessionId, "session-resolved");

  const done = deviceVoiceRouterReducer(pending, {
    type: "delivery",
    utteranceId: "utterance-a",
    ok: true,
    sessionId: "session-other",
    message: "sent",
    reply: "reply",
  });
  assert.equal(done.flow.phase, "done");
  assert.equal(done.flow.sessionId, "session-resolved");
  assert.equal(done.flow.reply, "reply");

  assert.strictEqual(deviceVoiceRouterReducer(done, {
    type: "delivery",
    utteranceId: "utterance-a",
    ok: false,
    message: "duplicate",
  }), done);
});

test("foreground-current route may resolve to the exact device session after guarded fallback", () => {
  const listening = transcript(DEVICE_VOICE_ROUTER_INITIAL_STATE, {
    utteranceId: "utterance-current",
    phase: "listening",
    agentId: "codex",
    sessionId: "current",
  });
  const fallback = transcript(listening, {
    utteranceId: "utterance-current",
    phase: "partial",
    revision: 1,
    text: "hello",
    agentId: "codex",
    sessionId: "session-from-device",
  });

  assert.equal(fallback.flow.agentId, "codex");
  assert.equal(fallback.flow.sessionId, "session-from-device");
});
