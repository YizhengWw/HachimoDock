/**
 * [Input] P4 Session-selection and presentation helpers.
 * [Output] Behavioral coverage for encoder cycling, unique targeting, and bounded card copy.
 * [Pos] Test node for dashboard-to-device Session synchronization.
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  P4_SESSION_LEASE_REFRESH_MS,
  cycleVoiceSessionId,
  formatDeviceSessionContent,
  formatDeviceSessionTitle,
  isDeviceSessionTargetUnique,
} from "./useP4SessionSync.js";

test("P4 Session lease refreshes safely inside the firmware timeout", () => {
  assert.equal(P4_SESSION_LEASE_REFRESH_MS, 4_000);
  assert.ok(P4_SESSION_LEASE_REFRESH_MS * 5 < 30_000);
});

test("P4 encoder cycling stays inside the exact visible Session list", () => {
  const sessions = [{ id: "a" }, { id: "b" }, { id: "c" }, { id: "b" }];
  assert.equal(cycleVoiceSessionId("auto", sessions, 1), "b");
  assert.equal(cycleVoiceSessionId("a", sessions, -1), "c");
  assert.equal(cycleVoiceSessionId("c", sessions, 1), "a");
  assert.equal(cycleVoiceSessionId("missing", sessions, 1), "a");
  assert.equal(cycleVoiceSessionId("auto", [], 1), "auto");
});

test("P4 Session targeting uses cwd only when duplicate titles require it", () => {
  const selected = { id: "a", name: "Build", cwd: "D:/one" };
  assert.equal(formatDeviceSessionTitle(selected), "Build");
  assert.equal(isDeviceSessionTargetUnique(selected, [selected]), true);
  assert.equal(isDeviceSessionTargetUnique(selected, [
    selected,
    { id: "b", name: "Build", cwd: "D:/two" },
  ]), true);
  assert.equal(isDeviceSessionTargetUnique(selected, [
    selected,
    { id: "b", name: "Build", cwd: "D:/one" },
  ]), false);
});

test("P4 card title accepts event-provided display metadata before ID fallback", () => {
  assert.equal(
    formatDeviceSessionTitle({ id: "codex-new", displayTitle: "Implement routing" }),
    "Implement routing",
  );
  assert.equal(formatDeviceSessionTitle({ id: "session-abcdef" }), "会话 session-");
});

test("P4 card content prefers live text, suppresses plugin payloads, and respects UTF-8 limits", () => {
  assert.equal(
    formatDeviceSessionContent({ displayContent: "正在构建", summary: "old" }, "Build"),
    "正在构建",
  );
  assert.equal(
    formatDeviceSessionContent({ summary: "<recommended_plugins>hidden" }, "Build"),
    "",
  );
  assert.equal(formatDeviceSessionContent({ summary: "Build" }, "Build"), "");
  assert.ok(
    new TextEncoder().encode(formatDeviceSessionContent({ summary: "测".repeat(200) })).length
      <= 383,
  );
});
