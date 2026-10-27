import test from "node:test";
import assert from "node:assert/strict";
import { isRecoveringAsr } from "./realtime-feedback.js";

test("ASR retry prompt remains a listening status, not a failed conversation", () => {
  const listening = { active: true, state: "listening", reason: "asr_timeout" };
  assert.equal(isRecoveringAsr(listening), true);
  for (const changed of [{ active: false }, { state: "ended" }, { reason: "asr" }, { reason: "config" }]) {
    assert.equal(isRecoveringAsr({ ...listening, ...changed }), false);
  }
  assert.equal(isRecoveringAsr(null), false);
});
