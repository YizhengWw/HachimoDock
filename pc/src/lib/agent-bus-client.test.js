/**
 * [Input] Mock local Agent Bus HTTP responses.
 * [Output] Contract coverage for status normalization and cursor request encoding.
 * [Pos] Test node for the Agent Bus transport client.
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  fetchAgentBusStatus,
  fetchAgentSessionEvents,
} from "./agent-bus-client.js";

test("Agent Bus client normalizes adapters and encodes ordered Session cursors", async () => {
  const previousFetch = globalThis.fetch;
  const requests = [];
  globalThis.fetch = async (url, options) => {
    requests.push({ url: String(url), signal: options?.signal });
    const body = String(url).includes("session-events")
      ? { cursor: 9, streamId: "stream-a", events: [{ sequence: 9 }] }
      : { ok: true, adapters: [{ agentId: "codex" }] };
    return { ok: true, status: 200, json: async () => body };
  };

  try {
    assert.deepEqual(await fetchAgentBusStatus(), {
      ok: true,
      agents: [{ agentId: "codex" }],
    });
    assert.deepEqual(
      await fetchAgentSessionEvents("claude code", 8, "stream/old"),
      {
        cursor: 9,
        streamId: "stream-a",
        reset: false,
        events: [{ sequence: 9 }],
      },
    );
    assert.match(requests[1].url, /agentId=claude\+code/);
    assert.match(requests[1].url, /cursor=8/);
    assert.match(requests[1].url, /streamId=stream%2Fold/);
    assert.equal(requests.every((request) => request.signal instanceof AbortSignal), true);
  } finally {
    globalThis.fetch = previousFetch;
  }
});
