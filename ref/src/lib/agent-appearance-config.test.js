/**
 * [Input] Agent-to-appearance assignment helper.
 * [Output] Node regression coverage for per-channel appearance binding plus single active desktop-channel behavior.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  activeDesktopAssignment,
  assignAppearanceToAgent,
  assignedAgentIds,
  ENABLED_AGENTS_STORAGE_KEY,
  loadEnabledAgents,
  normalizeDetectedAgents,
  pickFirstDetectedAgentId,
  sanitizeAgentAppearanceMap,
  shouldConfirmChannelSwitch,
  toSingleAgentSet,
} from "./agent-appearance-config.js";

function installStorage(initial = {}) {
  const values = new Map(Object.entries(initial));
  globalThis.localStorage = {
    getItem: (key) => values.get(key) || null,
    setItem: (key, value) => values.set(key, String(value)),
    removeItem: (key) => values.delete(key),
    clear: () => values.clear(),
  };
  return values;
}

test("assignAppearanceToAgent keeps one channel bound to at most one appearance", () => {
  const next = assignAppearanceToAgent(
    {
      codex: "builtin-terrier",
    },
    "codex",
    "custom-cloud",
  );

  assert.deepEqual(next, {
    codex: "custom-cloud",
  });
});

test("assignAppearanceToAgent preserves other channel appearance settings", () => {
  const next = assignAppearanceToAgent(
    {
      codex: "builtin-terrier",
      "claude-code": "custom-cloud",
    },
    "claude-code",
    "builtin-terrier",
  );

  assert.deepEqual(next, {
    codex: "builtin-terrier",
    "claude-code": "builtin-terrier",
  });
});

test("assignAppearanceToAgent clears only the selected channel when no appearance is selected", () => {
  const next = assignAppearanceToAgent(
    {
      codex: "builtin-terrier",
      "claude-code": "custom-cloud",
    },
    "claude-code",
    "",
  );

  assert.deepEqual(next, {
    codex: "builtin-terrier",
  });
});

test("sanitizeAgentAppearanceMap keeps valid per-channel assignments", () => {
  const next = sanitizeAgentAppearanceMap(
    {
      "claude-code": "custom-cloud",
      codex: "builtin-terrier",
    },
    [{ id: "builtin-terrier" }, { id: "custom-cloud" }],
  );

  assert.deepEqual(next, {
    "claude-code": "custom-cloud",
    codex: "builtin-terrier",
  });
});

test("channel switch warnings only appear when changing the current followed channel", () => {
  const map = { codex: "builtin-terrier", "claude-code": "custom-cloud" };

  assert.deepEqual(activeDesktopAssignment(map, new Set(["claude-code"])), {
    agentId: "claude-code",
    appearanceId: "custom-cloud",
  });
  assert.equal(shouldConfirmChannelSwitch(map, "claude-code", new Set(["claude-code"])), false);
  assert.equal(shouldConfirmChannelSwitch(map, "codex", new Set(["claude-code"])), true);
  assert.deepEqual(assignedAgentIds(map, "claude-code"), ["claude-code"]);
  assert.equal(shouldConfirmChannelSwitch({}, "claude-code"), false);
});

test("loadEnabledAgents keeps legacy values to a single active channel", () => {
  installStorage({
    [ENABLED_AGENTS_STORAGE_KEY]: JSON.stringify("claude-code"),
  });

  assert.deepEqual([...loadEnabledAgents()], ["claude-code"]);
});

test("normalizeDetectedAgents marks unavailable channels as not bindable", () => {
  const agents = normalizeDetectedAgents([
    { id: "codex", label: "Codex", detected: true, detail: "installed" },
  ]);

  assert.equal(agents.find((agent) => agent.id === "codex")?.detected, true);
  assert.equal(agents.find((agent) => agent.id === "openclaw")?.detected, false);
});

test("toSingleAgentSet respects stored order without preferring any specific agent", () => {
  assert.deepEqual([...toSingleAgentSet(["openclaw", "codex"])], ["openclaw"]);
  assert.deepEqual([...toSingleAgentSet(["claude-code", "codex"])], ["claude-code"]);
  assert.deepEqual([...toSingleAgentSet("openclaw")], ["openclaw"]);
  assert.deepEqual([...toSingleAgentSet([])], []);
});

test("pickFirstDetectedAgentId prefers the currently followed agent when it is detected", () => {
  const agents = [
    { id: "claude-code", detected: true },
    { id: "codex", detected: true },
    { id: "openclaw", detected: true },
  ];

  assert.equal(pickFirstDetectedAgentId(agents, "openclaw"), "openclaw");
  assert.equal(pickFirstDetectedAgentId(agents, "codex"), "codex");
});

test("pickFirstDetectedAgentId falls back to first detected when followed agent is missing or undetected", () => {
  const agents = [
    { id: "claude-code", detected: false },
    { id: "codex", detected: true },
    { id: "openclaw", detected: true },
  ];

  assert.equal(pickFirstDetectedAgentId(agents, "openclaw"), "openclaw");
  assert.equal(pickFirstDetectedAgentId(agents, "claude-code"), "codex");
  assert.equal(pickFirstDetectedAgentId(agents), "codex");
  assert.equal(pickFirstDetectedAgentId([]), "");
});
