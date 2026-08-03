/**
 * [Input] Pure P4 Session routing, lifecycle merge, dismissal, and transport helpers.
 * [Output] Behavioral regression coverage independent of React page source layout.
 * [Pos] test node for ref/src/lib/p4-session-service.js
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  buildP4ConversationQueue,
  buildP4DeviceSessionTransportPayload,
  buildP4RoutingSessions,
  filterDismissedP4Sessions,
  mergeP4SessionEvent,
  mergeP4SessionSnapshot,
  p4SessionHasMeaningfulTitle,
  p4SessionActivitySignature,
} from "./p4-session-service.js";

test("routing sessions deduplicate by id, sort by activity, and exclude internal review tasks", () => {
  const sessions = buildP4RoutingSessions([
    { id: "older", state: "working", lastModified: 10 },
    { id: "newer", state: "working", statusUpdatedAt: 40 },
    { id: "older", state: "thinking", lastModified: 30 },
    { id: "review", model: "codex-auto-review", statusUpdatedAt: 100 },
    {
      id: "approval",
      name: "The following is the Code",
      displayContent: '{"risk_level":"medium","user_authorization":"high","outcome":"allow","rationale":"ok"}',
      state: "done",
      statusUpdatedAt: 110,
    },
    { id: "   ", state: "working", statusUpdatedAt: 200 },
  ]);

  assert.deepEqual(sessions.map((session) => [session.id, session.state]), [
    ["newer", "working"],
    ["older", "thinking"],
  ]);
});

test("snapshots preserve event lifecycle fields while still removing omitted routing sessions", () => {
  const previous = [
    { id: "kept", state: "working", lastModified: 10, summary: "original" },
    { id: "updated", state: "working", lastModified: 20, summary: "before" },
  ];
  const snapshot = mergeP4SessionSnapshot(previous, [
    { id: "updated", lastModified: 30, summary: "snapshot" },
  ]);
  assert.deepEqual(snapshot.map((session) => session.id), ["updated"]);
  assert.equal(snapshot[0].state, "working");

  const event = mergeP4SessionEvent(snapshot, {
    id: "updated",
    state: "done",
    statusUpdatedAt: 40,
    displayContent: "finished",
  });
  assert.equal(event[0].state, "done");
  assert.equal(event[0].summary, "snapshot");
  assert.equal(event[0].displayContent, "finished");
  assert.equal(event[0].lastModified, 40);
});

test("authoritative queue reconciliation retains a missing active card as done", () => {
  const previous = buildP4ConversationQueue(
    [{ id: "app-development", name: "App development", state: "working" }],
    [],
    100,
  );
  const firstMiss = buildP4ConversationQueue(
    [],
    previous,
    200,
    { authoritativeSnapshot: true },
  );
  const next = buildP4ConversationQueue(
    [],
    firstMiss,
    15_201,
    { authoritativeSnapshot: true },
  );

  assert.equal(next.length, 1);
  assert.equal(next[0].state, "done");
});

test("titleless active events wait for stable snapshot metadata before creating a card", () => {
  const titleless = buildP4ConversationQueue(
    [{ id: "new-codex", state: "working", statusUpdatedAt: 100 }],
    [],
    100,
  );
  assert.deepEqual(titleless, []);
  assert.equal(p4SessionHasMeaningfulTitle({ displayTitle: "Codex 会话" }), false);

  const routing = mergeP4SessionSnapshot(
    [{ id: "new-codex", state: "working", statusUpdatedAt: 100 }],
    [{ id: "new-codex", summary: "Implement device routing", lastModified: 200 }],
  );
  const titled = buildP4ConversationQueue(routing, titleless, 200);
  assert.equal(titled.length, 1);
  assert.equal(titled[0].state, "working");
  assert.equal(titled[0].summary, "Implement device routing");
});

test("conversation queue admits active cards and exports only the remaining terminal TTL", () => {
  const nowMs = 1_000_000;
  const active = buildP4ConversationQueue(
    [{ id: "task", name: "Task", state: "working", statusUpdatedAt: nowMs }],
    [],
    nowMs,
  );
  const terminal = buildP4ConversationQueue(
    [{ id: "task", state: "done", statusUpdatedAt: nowMs + 1 }],
    active,
    nowMs + 1,
  );
  const payload = buildP4DeviceSessionTransportPayload(
    terminal.map((session) => ({
      ...session,
      terminalUntilMs: nowMs + 60_001,
    })),
    nowMs + 15_001,
  );

  assert.equal(payload.length, 1);
  assert.equal(payload[0].state, "done");
  assert.equal(payload[0].terminalRemainingMs, 45_000);
  assert.equal("terminalUntilMs" in payload[0], false);
});

test("dismissed sessions return only after their activity signature changes", () => {
  const initial = {
    id: "task",
    state: "working",
    statusUpdatedAt: 10,
    displayContent: "step one",
  };
  const dismissed = { task: p4SessionActivitySignature(initial) };

  assert.deepEqual(filterDismissedP4Sessions([initial], dismissed), []);
  assert.deepEqual(
    filterDismissedP4Sessions([{ ...initial, displayContent: "step two" }], dismissed)
      .map((session) => session.id),
    ["task"],
  );
});
