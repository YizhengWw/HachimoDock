/**
 * [Input] Detected local coding agents, persisted appearance records, and localStorage bridge assignment state.
 * [Output] Shared per-channel appearance mapping plus single active desktop-channel helpers used by setup, dashboard, and gallery.
 * [Pos] lib node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md` and consumers that persist agent assignments.
 */

import { BUILTIN_TERRIER_APPEARANCE_ID } from "./builtin-appearances.js";

export const AGENT_APPEARANCE_MAP_STORAGE_KEY = "pet-manager.agent-appearance-map";
export const ENABLED_AGENTS_STORAGE_KEY = "pet-manager.enabled-agents";

export const FIXED_AGENT_OPTIONS = [
  {
    id: "claude-code",
    label: "Claude Code",
    detail: "Claude Code 本地 CLI 渠道",
  },
  {
    id: "codex",
    label: "Codex",
    detail: "Codex 本地任务与状态渠道",
  },
  {
    id: "openclaw",
    label: "OpenClaw",
    detail: "OpenClaw 本地运行时渠道",
  },
];

export function loadEnabledAgents() {
  try {
    const raw = localStorage.getItem(ENABLED_AGENTS_STORAGE_KEY);
    if (raw) return toSingleAgentSet(JSON.parse(raw));
  } catch {}
  return null;
}

export function saveEnabledAgents(set) {
  localStorage.setItem(ENABLED_AGENTS_STORAGE_KEY, JSON.stringify([...set]));
}

export function normalizeDetectedAgents(agents = []) {
  const detectedById = new Map((agents || []).map((agent) => [agent.id, agent]));
  return FIXED_AGENT_OPTIONS.map((option) => {
    const detected = detectedById.get(option.id);
    return {
      ...option,
      ...(detected || {}),
      label: detected?.label || option.label,
      detail: detected?.detail || option.detail,
      detected: Boolean(detected?.detected),
    };
  });
}

export function toSingleAgentSet(value) {
  const values = typeof value === "string" ? [value] : Array.isArray(value) ? value : [...(value || [])];
  const first = values[0];
  return first ? new Set([first]) : new Set();
}

export function pickFirstDetectedAgentId(agents = [], preferredAgentId = "") {
  if (preferredAgentId) {
    const preferred = agents.find(
      (agent) => agent.id === preferredAgentId && agent.detected,
    );
    if (preferred) return preferred.id;
  }
  return agents.find((agent) => agent.detected)?.id || "";
}

export function defaultAgentAppearanceMap(records = []) {
  const defaultAppearance =
    records.find((record) => record.id === BUILTIN_TERRIER_APPEARANCE_ID) || records[0];
  if (!defaultAppearance) return {};
  return Object.fromEntries(FIXED_AGENT_OPTIONS.map((agent) => [agent.id, defaultAppearance.id]));
}

export function sanitizeAgentAppearanceMap(map, records = []) {
  const validAppearanceIds = new Set(records.map((record) => record.id));
  const validAgentIds = new Set(FIXED_AGENT_OPTIONS.map((agent) => agent.id));
  const validEntries = Object.entries(map || {}).filter(
    ([agentId, appearanceId]) =>
      validAgentIds.has(agentId) &&
      typeof appearanceId === "string" &&
      validAppearanceIds.has(appearanceId),
  );
  return Object.fromEntries(validEntries);
}

export function loadAgentAppearanceMap(records = []) {
  try {
    const raw = localStorage.getItem(AGENT_APPEARANCE_MAP_STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw);
      if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
        return sanitizeAgentAppearanceMap(parsed, records);
      }
    }
  } catch {}
  return defaultAgentAppearanceMap(records);
}

export function saveAgentAppearanceMap(map) {
  localStorage.setItem(AGENT_APPEARANCE_MAP_STORAGE_KEY, JSON.stringify(map));
}

export function assignedAgentIds(agentAppearanceMap, enabledAgents = null) {
  const active = activeDesktopAssignment(agentAppearanceMap, enabledAgents);
  return active.agentId ? [active.agentId] : [];
}

export function appearanceById(records, appearanceId) {
  return records.find((record) => record.id === appearanceId) || null;
}

export function channelLabelForId(agents, agentId) {
  return agents.find((agent) => agent.id === agentId)?.label || agentId;
}

export function firstAgentIdForAppearance(agentAppearanceMap, appearanceId) {
  return Object.entries(agentAppearanceMap || {}).find(([, id]) => id === appearanceId)?.[0] || "";
}

function firstEnabledAgentId(enabledAgents) {
  if (!enabledAgents) return "";
  const values = typeof enabledAgents === "string"
    ? [enabledAgents]
    : Array.isArray(enabledAgents)
      ? enabledAgents
      : [...enabledAgents];
  return values[0] || "";
}

export function activeDesktopAssignment(agentAppearanceMap, enabledAgents = null) {
  const enabledAgentId = firstEnabledAgentId(enabledAgents);
  if (enabledAgentId) {
    return {
      agentId: enabledAgentId,
      appearanceId: agentAppearanceMap?.[enabledAgentId] || BUILTIN_TERRIER_APPEARANCE_ID,
    };
  }

  const [agentId = "", appearanceId = ""] =
    Object.entries(agentAppearanceMap || {}).find(([, id]) => Boolean(id)) || [];
  return { agentId, appearanceId: appearanceId || (agentId ? BUILTIN_TERRIER_APPEARANCE_ID : "") };
}

export function shouldConfirmChannelSwitch(agentAppearanceMap, nextAgentId, enabledAgents = null) {
  const currentAgentId = activeDesktopAssignment(agentAppearanceMap, enabledAgents).agentId;
  return Boolean(currentAgentId && nextAgentId && currentAgentId !== nextAgentId);
}

export function assignAppearanceToAgent(map, agentId, appearanceId) {
  const next = { ...(map || {}) };
  if (!agentId) return next;
  if (!appearanceId) {
    delete next[agentId];
    return next;
  }
  next[agentId] = appearanceId;
  return next;
}
