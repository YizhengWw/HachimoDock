// Fake openclaw agent-runtime.js — emulates `agentCommand({...}, runtime)`
// well enough to drive OpenClawAdapter end-to-end in tests without ever
// installing the real openclaw package.
//
// Behavior knobs (env):
//   FAKE_OC_FAIL=1      throw, so the helper emits a top-level error
//   FAKE_OC_REPLY       full reply text (default: "你好")
//   FAKE_OC_NO_EVENTS=1 don't emit per-token events (simulates an install
//                       without pi-embedded event stream); helper should
//                       fall back to the result.text token.

export async function agentCommand(req, runtime) {
  if (process.env.FAKE_OC_FAIL === "1") {
    throw new Error("fake openclaw failure");
  }

  const reply = process.env.FAKE_OC_REPLY || "你好";
  const sessionId = req.sessionId || `fake-oc-${Math.floor(Math.random() * 1e6).toString(36)}`;

  return {
    payloads: [{ text: reply }],
    meta: { sessionId },
    sessionId,
  };
}
