/**
 * [Input] Consume agent discovery fixtures defined by `ref/src/fixtures.js`[Pos].
 * [Output] Provide the stage-one discovery contract, Tauri-aware discovery adapter, and normalization helpers for local agent detection.
 * [Pos] contract node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import { invoke } from "@tauri-apps/api/core";
import { AGENT_DISCOVERY_FIXTURES } from "./fixtures.js";

export const AGENT_STATUS = Object.freeze({
  ready: "ready",
  needsHook: "needs_hook",
  manualSetup: "manual_setup",
  notFound: "not_found",
});

export const AGENT_DISCOVERY_SCENARIOS = Object.freeze({
  readyAvailable: "ready_available",
  noReady: "no_ready",
  helperUnavailable: "helper_unavailable",
});

function normalizeText(value, fallback = "") {
  const normalized = String(value || "").trim();
  return normalized || String(fallback || "").trim();
}

function normalizeAgentStatus(value) {
  const normalized = normalizeText(value);
  return Object.values(AGENT_STATUS).includes(normalized)
    ? normalized
    : AGENT_STATUS.notFound;
}

function normalizeAgentRecord(rawAgent = {}) {
  const status = normalizeAgentStatus(rawAgent.status);
  const ready = status === AGENT_STATUS.ready && rawAgent.ready !== false;

  return {
    id: normalizeText(rawAgent.id),
    label: normalizeText(rawAgent.label),
    detected: Boolean(rawAgent.detected),
    ready,
    status,
    detail: normalizeText(rawAgent.detail),
    commandPath: normalizeText(rawAgent.commandPath),
    configPath: normalizeText(rawAgent.configPath),
    activityPath: normalizeText(rawAgent.activityPath),
    canSyncHook: Boolean(rawAgent.canSyncHook),
  };
}

function buildSummary(agents = []) {
  const readyAgents = agents.filter((agent) => agent.ready);
  const defaultSelectedAgentId = readyAgents[0]?.id || "";
  const needsAttentionCount = agents.filter((agent) => {
    return agent.status === AGENT_STATUS.needsHook || agent.status === AGENT_STATUS.manualSetup;
  }).length;

  return {
    readyCount: readyAgents.length,
    hasAnyReady: readyAgents.length > 0,
    defaultSelectedAgentId,
    needsAttentionCount,
  };
}

export function normalizeAgentDiscoveryResponse(rawResponse = {}) {
  const agents = Array.isArray(rawResponse.agents)
    ? rawResponse.agents.map(normalizeAgentRecord)
    : [];
  const computedSummary = buildSummary(agents);
  const sourceSummary = rawResponse.summary && typeof rawResponse.summary === "object"
    ? rawResponse.summary
    : {};

  return {
    scannedAt: Number(rawResponse.scannedAt) || Date.now(),
    agents,
    summary: {
      readyCount: Number(sourceSummary.readyCount ?? computedSummary.readyCount) || 0,
      hasAnyReady: Boolean(sourceSummary.hasAnyReady ?? computedSummary.hasAnyReady),
      defaultSelectedAgentId: normalizeText(
        sourceSummary.defaultSelectedAgentId,
        computedSummary.defaultSelectedAgentId,
      ),
      needsAttentionCount: Number(sourceSummary.needsAttentionCount ?? computedSummary.needsAttentionCount) || 0,
    },
  };
}

export function pickDefaultReadyAgent(discoveryResponse) {
  const response = normalizeAgentDiscoveryResponse(discoveryResponse);
  if (!response.summary.hasAnyReady) {
    return null;
  }

  return (
    response.agents.find((agent) => agent.id === response.summary.defaultSelectedAgentId && agent.ready) ||
    response.agents.find((agent) => agent.ready) ||
    null
  );
}

export function createConnectedAgentProfile(agent, options = {}) {
  if (!agent) {
    return {
      id: normalizeText(options.id, "unbound-agent"),
      title: normalizeText(options.title, "未连接 Agent"),
      workspace: normalizeText(options.workspace, "稍后再连接"),
      status: AGENT_STATUS.notFound,
    };
  }

  return {
    id: agent.id,
    title: agent.label,
    workspace: normalizeText(options.workspace, "当前本机工作区"),
    status: agent.status,
  };
}

export async function loadLocalAgents(options = {}) {
  const scenario = normalizeText(options.scenario, AGENT_DISCOVERY_SCENARIOS.readyAvailable);
  const delayMs = Number(options.delayMs ?? 720);

  if (typeof window !== "undefined" && window.__TAURI_INTERNALS__) {
    const response = await invoke("detect_local_agents");
    return normalizeAgentDiscoveryResponse(response);
  }

  await new Promise((resolve) => window.setTimeout(resolve, delayMs));

  if (scenario === AGENT_DISCOVERY_SCENARIOS.helperUnavailable) {
    throw new Error("本地发现助手尚未接入，请先查看产品契约或稍后再试。");
  }

  const fixture = AGENT_DISCOVERY_FIXTURES[scenario] || AGENT_DISCOVERY_FIXTURES.ready_available;
  return normalizeAgentDiscoveryResponse(fixture);
}
