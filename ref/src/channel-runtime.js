/**
 * [Input] Consume Tauri channel commands plus `ref/src/agent-discovery-contract.js`[Pos].
 * [Output] Provide a local-first runtime adapter for persisted channel config, agent-derived channel generation, and terminal-backed path verification.
 * [Pos] runtime node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import { invoke } from "@tauri-apps/api/core";
import { hasTauriRuntime } from "./lib/tauri-env.js";
import {
  AGENT_DISCOVERY_SCENARIOS,
  AGENT_STATUS,
  loadLocalAgents,
  normalizeAgentDiscoveryResponse,
} from "./agent-discovery-contract.js";

const WEB_STORAGE_KEY = "pet-manager.channel-config.v1";
const DEFAULT_TARGET = "电脑端 -> 副屏设备";

function safeReadStorage() {
  if (typeof window === "undefined" || !window.localStorage) {
    return [];
  }

  try {
    const raw = window.localStorage.getItem(WEB_STORAGE_KEY);
    const parsed = JSON.parse(raw || "[]");
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}

function safeWriteStorage(channels) {
  if (typeof window === "undefined" || !window.localStorage) {
    return;
  }

  window.localStorage.setItem(WEB_STORAGE_KEY, JSON.stringify(channels));
}

function mapAgentToChannelType(agentId) {
  return agentId === "codex" ? "jsonl-poller" : "local-agent";
}

function buildChannelSummary(agent) {
  switch (agent.status) {
    case AGENT_STATUS.ready:
      return "本机已检测到可用配置，可以作为实时 Channel。";
    case AGENT_STATUS.needsHook:
      return "已发现本机安装，但当前还缺少 hook，暂时不能启用。";
    case AGENT_STATUS.manualSetup:
      return "已发现本机安装，但当前仍需要手动配置 hook。";
    default:
      return "当前没有发现可用的本机配置。";
  }
}

function mergeDetectedChannels({ agents, persistedChannels = [] }) {
  const persistedMap = new Map(
    persistedChannels
      .filter((channel) => channel?.id)
      .map((channel) => [channel.id, channel]),
  );

  const detectedChannels = agents
    .filter((agent) => agent.detected)
    .map((agent) => {
      const persisted = persistedMap.get(agent.id);
      const desiredEnabled = typeof persisted?.enabled === "boolean" ? persisted.enabled : agent.ready;
      const enabled = Boolean(desiredEnabled && agent.ready);

      return {
        id: agent.id,
        label: agent.label,
        type: mapAgentToChannelType(agent.id),
        status: "disabled",
        enabled,
        target: DEFAULT_TARGET,
        summary: buildChannelSummary(agent),
        lastSync: "刚刚检测",
        sourceStatus: agent.status,
        detected: agent.detected,
        ready: agent.ready,
        detail: agent.detail,
        commandPath: agent.commandPath,
        configPath: agent.configPath,
        activityPath: agent.activityPath,
      };
    });

  const preferredPrimaryId = persistedChannels.find((channel) => channel?.isPrimary)?.id || "";
  const fallbackPrimaryId =
    detectedChannels.find((channel) => channel.id === preferredPrimaryId && channel.enabled)?.id ||
    detectedChannels.find((channel) => channel.enabled)?.id ||
    "";

  return detectedChannels.map((channel) => ({
    ...channel,
    status: !channel.enabled ? "disabled" : channel.id === fallbackPrimaryId ? "active" : "standby",
  }));
}

function normalizeWorkspaceResponse(rawWorkspace = {}) {
  const discovery = normalizeAgentDiscoveryResponse(rawWorkspace);
  const channels = Array.isArray(rawWorkspace.channels)
    ? rawWorkspace.channels.map((channel) => ({
        id: String(channel.id || "").trim(),
        label: String(channel.label || "").trim(),
        type: String(channel.type || channel.channelType || "").trim(),
        status: String(channel.status || "disabled").trim(),
        enabled: Boolean(channel.enabled),
        target: String(channel.target || DEFAULT_TARGET).trim(),
        summary: String(channel.summary || "").trim(),
        lastSync: String(channel.lastSync || "刚刚检测").trim(),
        sourceStatus: String(channel.sourceStatus || "").trim(),
        detected: Boolean(channel.detected),
        ready: Boolean(channel.ready),
        detail: String(channel.detail || "").trim(),
        commandPath: String(channel.commandPath || "").trim(),
        configPath: String(channel.configPath || "").trim(),
        activityPath: String(channel.activityPath || "").trim(),
      }))
    : [];

  return {
    scannedAt: Number(rawWorkspace.scannedAt || discovery.scannedAt || Date.now()),
    configPath: String(rawWorkspace.configPath || "").trim(),
    agents: discovery.agents,
    summary: discovery.summary,
    channels,
  };
}

function buildBrowserWorkspace(discovery, persistedChannels = []) {
  return {
    scannedAt: discovery.scannedAt,
    configPath: "browser://localStorage/pet-manager.channel-config.v1",
    agents: discovery.agents,
    summary: discovery.summary,
    channels: mergeDetectedChannels({ agents: discovery.agents, persistedChannels }),
  };
}

function serializeChannelConfig(channels = []) {
  return channels
    .filter((channel) => channel?.id)
    .map((channel) => ({
      id: channel.id,
      enabled: Boolean(channel.enabled),
      isPrimary: channel.status === "active",
    }));
}

export async function loadChannelWorkspace() {
  if (hasTauriRuntime()) {
    const workspace = await invoke("load_channel_workspace");
    return normalizeWorkspaceResponse(workspace);
  }

  const discovery = await loadLocalAgents({
    scenario: AGENT_DISCOVERY_SCENARIOS.readyAvailable,
    delayMs: 240,
  });
  return buildBrowserWorkspace(discovery, safeReadStorage());
}

export async function saveChannelConfig(channels = []) {
  if (hasTauriRuntime()) {
    const workspace = await invoke("save_channel_config", {
      channels: channels.map((channel) => ({
        id: channel.id,
        enabled: Boolean(channel.enabled),
        status: channel.status,
      })),
    });
    return normalizeWorkspaceResponse(workspace);
  }

  const persistedChannels = serializeChannelConfig(channels);
  safeWriteStorage(persistedChannels);

  const discovery = await loadLocalAgents({
    scenario: AGENT_DISCOVERY_SCENARIOS.readyAvailable,
    delayMs: 120,
  });
  return buildBrowserWorkspace(discovery, persistedChannels);
}

export async function verifyChannelPath({ path = "", expectedKind = "any" } = {}) {
  const trimmedPath = String(path || "").trim();
  const normalizedExpectedKind = String(expectedKind || "any").trim() || "any";

  if (!trimmedPath) {
    return {
      path: "",
      exists: false,
      valid: false,
      pathKind: "missing",
      executable: false,
      message: "当前还没有检测到可校验的本地路径。",
    };
  }

  if (hasTauriRuntime()) {
    return invoke("verify_local_path", {
      path: trimmedPath,
      expectedKind: normalizedExpectedKind,
    });
  }

  return {
    path: trimmedPath,
    exists: false,
    valid: false,
    pathKind: "unknown",
    executable: false,
    message: "浏览器预览模式无法校验本机路径，请在桌面端运行。",
  };
}
