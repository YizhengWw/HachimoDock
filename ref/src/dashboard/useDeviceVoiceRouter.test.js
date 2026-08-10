/**
 * [Input] Device voice transcript/progress/delivery state-machine actions.
 * [Output] Behavioral coverage for frozen/current-fallback routes, draft-ready confirmation, monotonic revisions, stale utterances, and one terminal delivery.
 * [Pos] Test node for the dashboard device-voice router.
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  DEVICE_VOICE_ROUTER_INITIAL_STATE,
  deviceVoiceRouterReducer,
} from "./useDeviceVoiceRouter.js";

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
  });
  assert.equal(draftReady.flow.phase, "draft_ready");

  const delayedPartial = transcript(draftReady, {
    utteranceId: "utterance-a",
    phase: "partial",
    revision: 4,
    text: "final text",
  });
  assert.equal(delayedPartial.flow.phase, "draft_ready");
  assert.equal(delayedPartial.flow.text, "final text");
  assert.equal(delayedPartial.flow.isFinal, true);

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
