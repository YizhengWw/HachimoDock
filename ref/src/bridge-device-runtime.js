/**
 * [Input] Consume Tauri bridge config commands and local preview fallback storage.
 * [Output] Provide a local-first runtime adapter for MQTT bridge `device_id`, local bridge status/start, direct MQTT follow-test publishing, retained cleanup, and Raspberry Pi board-runtime pet screen bindings with C-runtime topic/payload normalization.
 * [Pos] runtime node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import { invoke } from "@tauri-apps/api/core";

const WEB_STORAGE_KEY = "pet-manager.bridge-device-config.v1";
const WEB_CONFIG_PATH = "browser://localStorage/pet-manager.bridge-device-config.v1";
const WEB_REMOTE_BINDING_KEY = "pet-manager.remote-cli-binding.v1";
const WEB_REMOTE_CONFIG_PATH = "browser://localStorage/pet-manager.remote-cli-binding.v1";
const WEB_REMOTE_COMPANION_DRAFT_KEY = "pet-manager.remote-companion-control-draft.v1";
const WEB_REMOTE_COMPANION_RECORD_KEY = "pet-manager.remote-companion-control-record.v1";
const WEB_REMOTE_COMPANION_DRAFT_PATH = "browser://localStorage/pet-manager.remote-companion-control-draft.v1";
const WEB_REMOTE_COMPANION_RECORD_PATH = "browser://localStorage/pet-manager.remote-companion-control-record.v1";
const WEB_PET_SCREENS_KEY = "pet-manager.pet-screens.v1";
const WEB_PET_SCREEN_AP_SINK_KEY = "pet-manager.pet-screen-ap-sink.v1";
const DEFAULT_MQTT_NAMESPACE = "desk";
const DEFAULT_MQTT_BROKER_URL = "mqtt://broker.openclaw.example:1883";
const DEFAULT_TEST_DEVICE_ID = "linux-pet-01";
const REMOTE_CONTROL_TOPIC_SEGMENT = "control/remote-cli-binding";
const DEFAULT_PET_SCREEN_AP_BROKER_URL = "mqtt://192.168.44.1:1883";

function hasTauriRuntime() {
  return typeof window !== "undefined" && Boolean(window.__TAURI_INTERNALS__);
}

function normalizeText(value, fallback = "") {
  const normalized = String(value || "").trim();
  return normalized || String(fallback || "").trim();
}

function readBrowserConfig() {
  if (typeof window === "undefined" || !window.localStorage) {
    return {};
  }

  try {
    const raw = window.localStorage.getItem(WEB_STORAGE_KEY);
    const parsed = JSON.parse(raw || "{}");
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

function writeBrowserConfig(config = {}) {
  if (typeof window === "undefined" || !window.localStorage) {
    return;
  }

  window.localStorage.setItem(WEB_STORAGE_KEY, JSON.stringify(config));
}

function readBrowserRemoteBinding() {
  if (typeof window === "undefined" || !window.localStorage) {
    return {};
  }

  try {
    const raw = window.localStorage.getItem(WEB_REMOTE_BINDING_KEY);
    const parsed = JSON.parse(raw || "{}");
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

function writeBrowserRemoteBinding(config = {}) {
  if (typeof window === "undefined" || !window.localStorage) {
    return;
  }

  window.localStorage.setItem(WEB_REMOTE_BINDING_KEY, JSON.stringify(config));
}

function readBrowserRemoteCompanionDraft() {
  if (typeof window === "undefined" || !window.localStorage) {
    return {};
  }

  try {
    const raw = window.localStorage.getItem(WEB_REMOTE_COMPANION_DRAFT_KEY);
    const parsed = JSON.parse(raw || "{}");
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

function writeBrowserRemoteCompanionDraft(config = {}) {
  if (typeof window === "undefined" || !window.localStorage) {
    return;
  }

  window.localStorage.setItem(WEB_REMOTE_COMPANION_DRAFT_KEY, JSON.stringify(config));
}

function readBrowserRemoteCompanionRecord() {
  if (typeof window === "undefined" || !window.localStorage) {
    return {};
  }

  try {
    const raw = window.localStorage.getItem(WEB_REMOTE_COMPANION_RECORD_KEY);
    const parsed = JSON.parse(raw || "{}");
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

function writeBrowserRemoteCompanionRecord(config = {}) {
  if (typeof window === "undefined" || !window.localStorage) {
    return;
  }

  window.localStorage.setItem(WEB_REMOTE_COMPANION_RECORD_KEY, JSON.stringify(config));
}

function normalizeBridgeDeviceConfig(rawConfig = {}) {
  return {
    deviceId: normalizeText(rawConfig.deviceId),
    configPath: normalizeText(rawConfig.configPath, WEB_CONFIG_PATH),
    exists: Boolean(rawConfig.exists),
    source: normalizeText(rawConfig.source, "default"),
    detail: normalizeText(rawConfig.detail),
  };
}

function normalizeBridgeRuntimeStatus(rawConfig = {}) {
  const deviceId = normalizeTopicPart(rawConfig.deviceId, DEFAULT_TEST_DEVICE_ID);
  const mqttNamespace = normalizeTopicPart(rawConfig.mqttNamespace, DEFAULT_MQTT_NAMESPACE);
  const topicBase = `${mqttNamespace}/${deviceId}`;
  return {
    available: Boolean(rawConfig.available),
    mqttReady: Boolean(rawConfig.mqttReady),
    running: Boolean(rawConfig.running),
    workspaceRoot: normalizeText(rawConfig.workspaceRoot),
    entryPath: normalizeText(rawConfig.entryPath),
    configPath: normalizeText(rawConfig.configPath, WEB_CONFIG_PATH),
    brokerUrl: normalizeText(rawConfig.brokerUrl),
    mqttNamespace,
    deviceId,
    stateTopic: normalizeText(rawConfig.stateTopic, `${topicBase}/state/+`),
    speechTopic: normalizeText(rawConfig.speechTopic, `${topicBase}/speech/text`),
    port: Number.isFinite(Number(rawConfig.port)) ? Number(rawConfig.port) : 23333,
    command: normalizeText(rawConfig.command),
    detail: normalizeText(rawConfig.detail),
  };
}

function normalizeBridgeRuntimeStartResponse(rawConfig = {}) {
  return {
    ok: Boolean(rawConfig.ok),
    started: Boolean(rawConfig.started),
    status: normalizeBridgeRuntimeStatus(rawConfig.status),
    detail: normalizeText(rawConfig.detail),
  };
}

function normalizeMqttConfig(rawConfig = {}) {
  return {
    brokerUrl: normalizeText(rawConfig.brokerUrl),
    configPath: normalizeText(rawConfig.configPath, "browser://localStorage/pet-manager.mqtt-config.v1"),
    exists: Boolean(rawConfig.exists),
    source: normalizeText(rawConfig.source, "missing"),
    detail: normalizeText(rawConfig.detail),
  };
}

function buildBrowserBridgeConfig() {
  const stored = readBrowserConfig();
  const deviceId = normalizeText(stored.deviceId);

  return normalizeBridgeDeviceConfig({
    deviceId,
    configPath: WEB_CONFIG_PATH,
    exists: Boolean(deviceId),
    source: deviceId ? "local-storage" : "default",
    detail: deviceId
      ? "浏览器预览会把 bridge device_id 暂存在 localStorage 里。"
      : "浏览器预览还没有保存 bridge device_id，会继续沿用页面里的默认 desktop_device_id。",
  });
}

function normalizeTimestamp(value) {
  const numeric = Number(value);
  return Number.isFinite(numeric) && numeric > 0 ? numeric : 0;
}

function normalizeTopicPart(value, fallback = "") {
  const normalized = normalizeText(value)
    .replaceAll(/[^a-zA-Z0-9._-]/g, "-")
    .replaceAll(/-+/g, "-")
    .replace(/^-+|-+$/g, "");
  return normalized || normalizeText(fallback);
}

function normalizeRemoteCliBinding(rawConfig = {}) {
  return {
    enabled: Boolean(rawConfig.enabled),
    targetDeviceId: normalizeText(rawConfig.targetDeviceId),
    targetSource: normalizeText(rawConfig.targetSource),
    mqttNamespace: normalizeText(rawConfig.mqttNamespace, DEFAULT_MQTT_NAMESPACE),
    updatedAt: normalizeTimestamp(rawConfig.updatedAt),
    updatedBy: normalizeText(rawConfig.updatedBy),
    configPath: normalizeText(rawConfig.configPath, WEB_REMOTE_CONFIG_PATH),
    exists: Boolean(rawConfig.exists),
    topicBase: normalizeText(rawConfig.topicBase),
    detail: normalizeText(rawConfig.detail),
  };
}

function normalizeRemoteCompanionDraft(rawConfig = {}) {
  return {
    enabled: Boolean(rawConfig.enabled),
    recipientDeviceId: normalizeText(rawConfig.recipientDeviceId),
    sourceOverride: normalizeText(rawConfig.sourceOverride),
    targetSource: "",
    mqttNamespace: normalizeTopicPart(rawConfig.mqttNamespace, DEFAULT_MQTT_NAMESPACE),
    configPath: WEB_REMOTE_COMPANION_DRAFT_PATH,
  };
}

function buildControlTopic(recipientDeviceId, mqttNamespace = DEFAULT_MQTT_NAMESPACE) {
  const normalizedRecipient = normalizeText(recipientDeviceId);
  if (!normalizedRecipient) {
    return "";
  }

  return `${mqttNamespace}/${normalizedRecipient}/${REMOTE_CONTROL_TOPIC_SEGMENT}`;
}

function readBrowserPetScreensStore() {
  if (typeof window === "undefined" || !window.localStorage) {
    return { screens: [], activeBoardDeviceId: "" };
  }

  try {
    const raw = window.localStorage.getItem(WEB_PET_SCREENS_KEY);
    const parsed = JSON.parse(raw || "{}");
    return parsed && typeof parsed === "object" ? {
      screens: Array.isArray(parsed.screens) ? parsed.screens : [],
      activeBoardDeviceId: normalizeTopicPart(parsed.activeBoardDeviceId || parsed.activeScreenId),
    } : { screens: [], activeBoardDeviceId: "" };
  } catch {
    return { screens: [], activeBoardDeviceId: "" };
  }
}

function writeBrowserPetScreensStore(store) {
  if (typeof window === "undefined" || !window.localStorage) {
    return;
  }
  window.localStorage.setItem(WEB_PET_SCREENS_KEY, JSON.stringify(store));
}

function normalizePetScreenBinding(raw = {}) {
  const boardDeviceId = normalizeTopicPart(
    raw.boardDeviceId || raw.localDeviceId || raw.deviceId || raw.screenId,
  );
  const mode = normalizeText(raw.mode, "sta").toLowerCase() === "ap" ? "ap" : "sta";
  const host = normalizeText(raw.host, mode === "ap" ? "192.168.44.1" : "");
  const brokerUrl = normalizeText(
    raw.brokerUrl,
    mode === "ap"
      ? DEFAULT_PET_SCREEN_AP_BROKER_URL
      : host
        ? `mqtt://${host}:1883`
        : DEFAULT_MQTT_BROKER_URL,
  );
  return {
    boardDeviceId,
    name: normalizeText(raw.name, "Desk Pet Screen"),
    host,
    mode,
    brokerUrl,
    desktopDeviceId: normalizeTopicPart(raw.desktopDeviceId),
    mqttNamespace: normalizeTopicPart(raw.mqttNamespace, DEFAULT_MQTT_NAMESPACE),
  };
}

function normalizePetScreenCandidate(raw = {}) {
  return {
    ...normalizePetScreenBinding(raw),
    reachable: Boolean(raw.reachable),
    source: normalizeText(raw.source, "manual"),
    detail: normalizeText(raw.detail),
  };
}

function normalizePetScreenConnectionStatus(raw = {}) {
  return {
    boardDeviceId: normalizeTopicPart(raw.boardDeviceId),
    ok: Boolean(raw.ok),
    brokerUrl: normalizeText(raw.brokerUrl),
    stateTopic: normalizeText(raw.stateTopic),
    speechTopic: normalizeText(raw.speechTopic),
    detail: normalizeText(raw.detail),
    testedAt: normalizeTimestamp(raw.testedAt),
  };
}

function normalizePetScreenApSinkResponse(raw = {}) {
  return {
    enabled: Boolean(raw.enabled),
    boardDeviceId: normalizeTopicPart(raw.boardDeviceId),
    boardBrokerUrl: normalizeText(raw.boardBrokerUrl, DEFAULT_PET_SCREEN_AP_BROKER_URL),
    desktopDeviceId: normalizeTopicPart(raw.desktopDeviceId),
    mqttNamespace: normalizeTopicPart(raw.mqttNamespace, DEFAULT_MQTT_NAMESPACE),
    configPath: normalizeText(raw.configPath, `browser://localStorage/${WEB_PET_SCREEN_AP_SINK_KEY}`),
    detail: normalizeText(raw.detail),
  };
}

function normalizeRemoteCompanionDispatchRecord(rawConfig = {}) {
  return {
    enabled: Boolean(rawConfig.enabled),
    recipientDeviceId: normalizeText(rawConfig.recipientDeviceId),
    targetDeviceId: normalizeText(rawConfig.targetDeviceId),
    targetSource: normalizeText(rawConfig.targetSource),
    mqttNamespace: normalizeText(rawConfig.mqttNamespace, DEFAULT_MQTT_NAMESPACE),
    controlTopic: normalizeText(rawConfig.controlTopic),
    sourceTopicBase: normalizeText(rawConfig.sourceTopicBase),
    sourceStateTopic: normalizeText(rawConfig.sourceStateTopic),
    sourceSpeechTopic: normalizeText(rawConfig.sourceSpeechTopic),
    dispatchedAt: normalizeTimestamp(rawConfig.dispatchedAt),
    updatedBy: normalizeText(rawConfig.updatedBy),
    detail: normalizeText(rawConfig.detail),
    configPath: WEB_REMOTE_COMPANION_RECORD_PATH,
  };
}

function normalizeMqttBridgeSourceCandidate(rawConfig = {}) {
  const deviceId = normalizeTopicPart(rawConfig.deviceId);
  const mqttNamespace = normalizeTopicPart(rawConfig.mqttNamespace, DEFAULT_MQTT_NAMESPACE);
  const topicBase = normalizeText(rawConfig.topicBase, deviceId ? `${mqttNamespace}/${deviceId}` : "");
  return {
    deviceId,
    mqttNamespace,
    topicBase,
    stateTopic: normalizeText(rawConfig.stateTopic, topicBase ? `${topicBase}/state/+` : ""),
    activeStateTopic: normalizeText(rawConfig.activeStateTopic, topicBase ? `${topicBase}/state/active` : ""),
    speechTopic: normalizeText(rawConfig.speechTopic, topicBase ? `${topicBase}/speech/text` : ""),
    availabilityTopic: normalizeText(rawConfig.availabilityTopic, topicBase ? `${topicBase}/availability/bridge` : ""),
    sources: Array.isArray(rawConfig.sources)
      ? rawConfig.sources.map((source) => normalizeTopicPart(source)).filter(Boolean)
      : [],
    lastSource: normalizeTopicPart(rawConfig.lastSource),
    lastState: normalizeText(rawConfig.lastState),
    lastEvent: normalizeText(rawConfig.lastEvent),
    lastReason: normalizeText(rawConfig.lastReason),
    lastTsMs: normalizeTimestamp(rawConfig.lastTsMs),
    bridgeOnline: Boolean(rawConfig.bridgeOnline),
    retained: Boolean(rawConfig.retained),
    detail: normalizeText(rawConfig.detail),
  };
}

function normalizeFollowTestState(value) {
  const normalized = normalizeText(value, "working")
    .toLowerCase()
    .replaceAll(/[\s-]+/g, "_");
  if (normalized === "thinking" || normalized === "tool_running") return "working";
  return [
    "idle",
    "working",
    "speaking",
    "error",
    "waiting_user",
  ].includes(normalized)
    ? normalized
    : "working";
}

function normalizeMqttFollowTestResponse(rawConfig = {}) {
  const deviceId = normalizeTopicPart(rawConfig.deviceId, DEFAULT_TEST_DEVICE_ID);
  const mqttNamespace = normalizeTopicPart(rawConfig.mqttNamespace, DEFAULT_MQTT_NAMESPACE);
  const topicBase = `${mqttNamespace}/${deviceId}`;
  return {
    ok: Boolean(rawConfig.ok),
    deviceId,
    mqttNamespace,
    brokerUrl: normalizeText(rawConfig.brokerUrl, "browser://mqtt-preview"),
    stateTopic: normalizeText(rawConfig.stateTopic, `${topicBase}/state/pet-manager-test`),
    speechTopic: normalizeText(rawConfig.speechTopic, `${topicBase}/speech/text`),
    state: normalizeFollowTestState(rawConfig.state),
    speechText: normalizeText(rawConfig.speechText, "Pet Manager 已连接 MQTT"),
    publishedAt: normalizeTimestamp(rawConfig.publishedAt),
    detail: normalizeText(rawConfig.detail),
  };
}

function buildBrowserRemoteCliBinding() {
  const stored = readBrowserRemoteBinding();
  const targetDeviceId = normalizeText(stored.targetDeviceId);
  const targetSource = normalizeText(stored.targetSource);
  const mqttNamespace = normalizeText(stored.mqttNamespace, DEFAULT_MQTT_NAMESPACE);
  const enabled = Boolean(stored.enabled) && Boolean(targetDeviceId);

  return normalizeRemoteCliBinding({
    enabled,
    targetDeviceId,
    targetSource,
    mqttNamespace,
    updatedAt: normalizeTimestamp(stored.updatedAt),
    updatedBy: normalizeText(stored.updatedBy, "browser-preview"),
    configPath: WEB_REMOTE_CONFIG_PATH,
    exists: Boolean(targetDeviceId || stored.updatedAt),
    topicBase: enabled ? `${mqttNamespace}/${targetDeviceId}` : "",
    detail: enabled
      ? `浏览器预览会把远端桥接目标暂存在 localStorage 里。当前桌宠会从 ${mqttNamespace}/${targetDeviceId}/state/${targetSource || "+"} 接状态。`
      : targetDeviceId
        ? "浏览器预览已经保存了远端电脑草稿，但当前未启用覆盖。"
        : "浏览器预览还没有保存远端桥接目标，会继续沿用本机 desktop_device_id。",
  });
}

export async function loadBridgeDeviceConfig() {
  if (hasTauriRuntime()) {
    const response = await invoke("load_bridge_device_config");
    return normalizeBridgeDeviceConfig(response);
  }

  return buildBrowserBridgeConfig();
}

export async function saveBridgeDeviceConfig(deviceId) {
  const normalizedDeviceId = normalizeTopicPart(deviceId);

  if (hasTauriRuntime()) {
    const response = await invoke("save_bridge_device_config", {
      payload: {
        deviceId: normalizedDeviceId,
      },
    });
    return normalizeBridgeDeviceConfig(response);
  }

  writeBrowserConfig({ deviceId: normalizedDeviceId });
  return buildBrowserBridgeConfig();
}

export async function loadBridgeRuntimeStatus() {
  if (hasTauriRuntime()) {
    const response = await invoke("load_bridge_runtime_status");
    return normalizeBridgeRuntimeStatus(response);
  }

  const bridgeConfig = buildBrowserBridgeConfig();
  return normalizeBridgeRuntimeStatus({
    available: false,
    mqttReady: false,
    running: false,
    deviceId: bridgeConfig.deviceId || DEFAULT_TEST_DEVICE_ID,
    mqttNamespace: DEFAULT_MQTT_NAMESPACE,
    brokerUrl: "browser://mqtt-preview",
    configPath: bridgeConfig.configPath,
    detail: "浏览器预览不会启动 pet-claw bridge；请用 Tauri 桌面版执行真实链路。",
  });
}

export async function startBridgeMqtt() {
  if (hasTauriRuntime()) {
    const response = await invoke("start_bridge_mqtt");
    return normalizeBridgeRuntimeStartResponse(response);
  }

  const status = await loadBridgeRuntimeStatus();
  return normalizeBridgeRuntimeStartResponse({
    ok: false,
    started: false,
    status,
    detail: "浏览器预览不会启动 pet-claw bridge；请用 Tauri 桌面版执行真实链路。",
  });
}

export async function loadMqttConfig() {
  if (hasTauriRuntime()) {
    const response = await invoke("load_mqtt_config");
    return normalizeMqttConfig(response);
  }

  return normalizeMqttConfig({
    brokerUrl: "browser://mqtt-preview",
    source: "browser-preview",
    detail: "浏览器预览不会连接 MQTT broker；请用 Tauri 桌面版保存真实 broker。",
  });
}

export async function saveMqttConfig(brokerUrl) {
  const normalizedBrokerUrl = normalizeText(brokerUrl);
  if (hasTauriRuntime()) {
    const response = await invoke("save_mqtt_config", {
      payload: {
        brokerUrl: normalizedBrokerUrl,
      },
    });
    return normalizeMqttConfig(response);
  }

  return normalizeMqttConfig({
    brokerUrl: normalizedBrokerUrl || "browser://mqtt-preview",
    exists: Boolean(normalizedBrokerUrl),
    source: "browser-preview",
    detail: "浏览器预览不会保存真实 MQTT broker；请用 Tauri 桌面版执行。",
  });
}

export async function publishMqttFollowTest(payload = {}) {
  const bridgeConfig = buildBrowserBridgeConfig();
  const normalizedPayload = {
    deviceId: normalizeTopicPart(payload.deviceId, bridgeConfig.deviceId || DEFAULT_TEST_DEVICE_ID),
    state: normalizeFollowTestState(payload.state),
    speechText: normalizeText(payload.speechText, "Pet Manager 已连接 MQTT"),
  };

  if (hasTauriRuntime()) {
    const response = await invoke("publish_mqtt_follow_test", {
      payload: normalizedPayload,
    });
    return normalizeMqttFollowTestResponse(response);
  }

  const simulated = normalizeMqttFollowTestResponse({
    ...normalizedPayload,
    ok: false,
    mqttNamespace: DEFAULT_MQTT_NAMESPACE,
    brokerUrl: "browser://mqtt-preview",
    publishedAt: Date.now(),
    detail: "浏览器预览不会真的连接 MQTT，只展示将要发布给板子的 topic；请用 Tauri 桌面版执行真实测试。",
  });
  return simulated;
}

export async function loadRemoteCliBindingConfig() {
  if (hasTauriRuntime()) {
    const response = await invoke("load_remote_cli_binding_config");
    return normalizeRemoteCliBinding(response);
  }

  return buildBrowserRemoteCliBinding();
}

export async function saveRemoteCliBindingConfig(payload = {}) {
  const normalizedPayload = {
    enabled: Boolean(payload.enabled),
    targetDeviceId: normalizeText(payload.targetDeviceId),
    targetSource: normalizeText(payload.targetSource),
    mqttNamespace: normalizeText(payload.mqttNamespace, DEFAULT_MQTT_NAMESPACE),
    updatedBy: normalizeText(payload.updatedBy, "pet-manager"),
  };

  if (hasTauriRuntime()) {
    const response = await invoke("save_remote_cli_binding_config", {
      payload: normalizedPayload,
    });
    return normalizeRemoteCliBinding(response);
  }

  writeBrowserRemoteBinding({
    ...normalizedPayload,
    updatedAt: Date.now(),
  });
  return buildBrowserRemoteCliBinding();
}

export function loadRemoteCompanionControlDraft() {
  return normalizeRemoteCompanionDraft(readBrowserRemoteCompanionDraft());
}

export function saveRemoteCompanionControlDraft(payload = {}) {
  const normalizedPayload = normalizeRemoteCompanionDraft(payload);
  writeBrowserRemoteCompanionDraft(normalizedPayload);
  return normalizedPayload;
}

export function loadRemoteCompanionControlRecord() {
  return normalizeRemoteCompanionDispatchRecord(readBrowserRemoteCompanionRecord());
}

export async function scanMqttBridgeSources() {
  if (hasTauriRuntime()) {
    const response = await invoke("scan_mqtt_bridge_sources");
    return Array.isArray(response) ? response.map(normalizeMqttBridgeSourceCandidate) : [];
  }

  const bridgeConfig = buildBrowserBridgeConfig();
  return bridgeConfig.deviceId
    ? [
        normalizeMqttBridgeSourceCandidate({
          deviceId: bridgeConfig.deviceId,
          mqttNamespace: DEFAULT_MQTT_NAMESPACE,
          sources: ["active"],
          bridgeOnline: false,
          retained: false,
          detail: "浏览器预览不会连接 MQTT broker，只能展示本地暂存的 device_id。",
        }),
      ]
    : [];
}

export async function dispatchRemoteCliBindingCommand(payload = {}) {
  const normalizedPayload = {
    enabled: Boolean(payload.enabled),
    recipientDeviceId: normalizeText(payload.recipientDeviceId),
    targetDeviceId: normalizeText(payload.targetDeviceId),
    targetSource: normalizeText(payload.targetSource),
    mqttNamespace: normalizeTopicPart(payload.mqttNamespace, DEFAULT_MQTT_NAMESPACE),
    updatedBy: normalizeText(payload.updatedBy, "pet-manager"),
  };

  if (hasTauriRuntime()) {
    const response = await invoke("dispatch_remote_cli_binding_command", {
      payload: normalizedPayload,
    });
    const normalizedResponse = normalizeRemoteCompanionDispatchRecord(response);
    writeBrowserRemoteCompanionRecord(normalizedResponse);
    return normalizedResponse;
  }

  const sourceTopicBase = normalizedPayload.enabled && normalizedPayload.targetDeviceId
    ? `${normalizedPayload.mqttNamespace}/${normalizedPayload.targetDeviceId}`
    : "";
  const sourceStateTopic = normalizedPayload.enabled && sourceTopicBase
    ? `${sourceTopicBase}/state/${normalizedPayload.targetSource || "+"}`
    : "";
  const sourceSpeechTopic = normalizedPayload.enabled && sourceTopicBase
    ? `${sourceTopicBase}/speech/text`
    : "";
  const simulatedResponse = normalizeRemoteCompanionDispatchRecord({
    ...normalizedPayload,
    controlTopic: buildControlTopic(normalizedPayload.recipientDeviceId, normalizedPayload.mqttNamespace),
    sourceTopicBase,
    sourceStateTopic,
    sourceSpeechTopic,
    dispatchedAt: Date.now(),
    detail: normalizedPayload.enabled
      ? "浏览器预览不会真的发 MQTT，只会把将要下发到 Linux 桌宠的控制指令暂存在 localStorage。"
      : "浏览器预览不会真的发 MQTT，只会把将要下发的停止跟随指令暂存在 localStorage。",
    configPath: WEB_REMOTE_COMPANION_RECORD_PATH,
  });

  writeBrowserRemoteCompanionRecord(simulatedResponse);
  return simulatedResponse;
}

export async function scanPetScreens() {
  if (hasTauriRuntime()) {
    const response = await invoke("scan_pet_screens");
    return Array.isArray(response) ? response.map(normalizePetScreenCandidate) : [];
  }

  const store = readBrowserPetScreensStore();
  return store.screens.map((screen) => normalizePetScreenCandidate({
    ...screen,
    source: "local-storage",
    detail: "浏览器预览只展示已保存的硬件屏，不会扫描 MQTT broker。",
  }));
}

export async function clearDiscoveryCache() {
  if (hasTauriRuntime()) {
    return invoke("clear_discovery_cache");
  }

  const store = readBrowserPetScreensStore();
  writeBrowserPetScreensStore({ screens: [], activeBoardDeviceId: "" });
  return {
    clearedLocalScreens: store.screens.length,
    clearedRetainedTopics: 0,
    retainedError: "",
    detail: "浏览器预览已清空本地硬件板子历史；不会连接 MQTT broker 清理 retained 发现/控制消息。",
  };
}

export async function listPetScreenBindings() {
  if (hasTauriRuntime()) {
    const response = await invoke("list_pet_screen_bindings");
    return Array.isArray(response) ? response.map(normalizePetScreenBinding) : [];
  }

  return readBrowserPetScreensStore().screens.map(normalizePetScreenBinding);
}

export async function savePetScreenBinding(input = {}) {
  const binding = normalizePetScreenBinding(input);
  if (hasTauriRuntime()) {
    const response = await invoke("save_pet_screen_binding", {
      input: binding,
    });
    return normalizePetScreenBinding(response);
  }

  const store = readBrowserPetScreensStore();
  const nextScreens = store.screens.filter((screen) => normalizeText(screen.boardDeviceId) !== binding.boardDeviceId);
  nextScreens.push(binding);
  writeBrowserPetScreensStore({
    screens: nextScreens,
    activeBoardDeviceId: store.activeBoardDeviceId || binding.boardDeviceId,
  });
  return binding;
}

export async function activatePetScreen(boardDeviceId) {
  const normalizedBoardDeviceId = normalizeTopicPart(boardDeviceId);
  if (hasTauriRuntime()) {
    const response = await invoke("activate_pet_screen", {
      input: {
        boardDeviceId: normalizedBoardDeviceId,
      },
    });
    return normalizePetScreenBinding(response);
  }

  const store = readBrowserPetScreensStore();
  const found = store.screens.find((screen) => normalizeText(screen.boardDeviceId) === normalizedBoardDeviceId);
  if (!found) {
    throw new Error(`未找到硬件屏绑定: ${normalizedBoardDeviceId}`);
  }
  writeBrowserPetScreensStore({
    ...store,
    activeBoardDeviceId: normalizedBoardDeviceId,
  });
  return normalizePetScreenBinding(found);
}

export async function testPetScreenConnection(boardDeviceId) {
  const normalizedBoardDeviceId = normalizeTopicPart(boardDeviceId);
  if (hasTauriRuntime()) {
    const response = await invoke("test_pet_screen_connection", {
      input: {
        boardDeviceId: normalizedBoardDeviceId,
      },
    });
    return normalizePetScreenConnectionStatus(response);
  }

  const binding = readBrowserPetScreensStore().screens
    .map(normalizePetScreenBinding)
    .find((screen) => screen.boardDeviceId === normalizedBoardDeviceId);
  if (!binding) {
    throw new Error(`未找到硬件屏绑定: ${normalizedBoardDeviceId}`);
  }
  return normalizePetScreenConnectionStatus({
    boardDeviceId: binding.boardDeviceId,
    ok: true,
    brokerUrl: binding.brokerUrl,
    stateTopic: `${binding.mqttNamespace}/${binding.desktopDeviceId}/state/pet-manager-test`,
    speechTopic: `${binding.mqttNamespace}/${binding.desktopDeviceId}/speech/text`,
    testedAt: Date.now(),
    detail: "浏览器预览不会真的发布 MQTT，只模拟测试成功。",
  });
}

export async function configurePetScreenApSink(payload = {}) {
  const normalizedPayload = {
    boardDeviceId: normalizeTopicPart(payload.boardDeviceId),
    boardBrokerUrl: normalizeText(payload.boardBrokerUrl, DEFAULT_PET_SCREEN_AP_BROKER_URL),
    desktopDeviceId: normalizeTopicPart(payload.desktopDeviceId),
  };

  if (hasTauriRuntime()) {
    const response = await invoke("configure_pet_screen_ap_sink", {
      input: normalizedPayload,
    });
    return normalizePetScreenApSinkResponse(response);
  }

  const simulatedResponse = normalizePetScreenApSinkResponse({
    ...normalizedPayload,
    enabled: true,
    mqttNamespace: DEFAULT_MQTT_NAMESPACE,
    detail: "浏览器预览只会保存 AP fallback 副发布器配置，不会改动运行中的 bridge。",
  });
  if (typeof window !== "undefined" && window.localStorage) {
    window.localStorage.setItem(WEB_PET_SCREEN_AP_SINK_KEY, JSON.stringify(simulatedResponse));
  }
  return simulatedResponse;
}

// --- USB direct connection ---

export async function scanUsbDevices() {
  if (hasTauriRuntime()) {
    return invoke("usb_scan_devices");
  }
  return [];
}

export async function connectUsb(portName) {
  if (hasTauriRuntime()) {
    return invoke("usb_connect", { portName });
  }
  throw new Error("USB direct connection is only available in desktop mode");
}

export async function disconnectUsb() {
  if (hasTauriRuntime()) {
    return invoke("usb_disconnect");
  }
}

export async function usbSendState(source, payload) {
  if (hasTauriRuntime()) {
    return invoke("usb_send_state", { source, payload });
  }
  throw new Error("USB direct connection is only available in desktop mode");
}

export async function usbSendSpeech(text) {
  if (hasTauriRuntime()) {
    return invoke("usb_send_speech", { text });
  }
  throw new Error("USB direct connection is only available in desktop mode");
}

export async function usbSendCommand(command) {
  if (hasTauriRuntime()) {
    return invoke("usb_send_command", { command });
  }
  throw new Error("USB direct connection is only available in desktop mode");
}

export async function getUsbStatus() {
  if (hasTauriRuntime()) {
    return invoke("usb_get_status");
  }
  return { connected: false, portName: "", boardDeviceId: "", transport: "mqtt" };
}

export async function getConnectionMode() {
  if (hasTauriRuntime()) {
    const status = await invoke("usb_get_status");
    return status.connected ? "usb" : "mqtt";
  }
  return "mqtt";
}
