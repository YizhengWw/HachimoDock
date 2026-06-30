#!/usr/bin/env node
"use strict";

/*
 * [Input] Local agent state monitors, bridge profile env, MQTT broker events, device availability, and mock/board voice inject requests.
 * [Output] Per-source retained MQTT status, USB-forwarder state files, remote board binding commands, and agent-session injections with fresh-session recovery for stale Codex metadata.
 * [Pos] Headless status bridge for the Tauri Pet Manager runtime.
 * [Sync] If state-file, follow-source, or voice-injection recovery semantics change, update `ref/.folder.md`.
 */

const crypto = require("crypto");
const fs = require("fs");
const http = require("http");
const os = require("os");
const path = require("path");
const process = require("process");

const {
  CLAWD_SERVER_HEADER,
  CLAWD_SERVER_ID,
  DEFAULT_SERVER_PORT,
  clearRuntimeConfig,
  getPortCandidates,
  readRuntimePort,
  writeRuntimeConfig,
} = require("../../../hooks/server-config");
const { SessionMetricsTracker } = require("./status-metrics");

const APP_NAME = "clawd-status-bridge";
const APP_VERSION = "0.1.0";
const PROTOCOL_VERSION_FALLBACK = 3;
const SPEECH_EXPIRES_MS = 30000;
const ED25519_SPKI_PREFIX = Buffer.from("302a300506032b6570032100", "hex");
const STATUS_VALUES = new Set(["idle", "working", "speaking", "done", "error", "waiting_user"]);
const LEGACY_WORKING_STATUS_VALUES = new Set(["thinking", "tool_running"]);
const KNOWN_AGENT_SOURCES = ["codex", "claude-code", "openclaw"];

function requireOptional(name) {
  try {
    return require(name);
  } catch (error) {
    return null;
  }
}

const mqtt = requireOptional("mqtt");
const WebSocket = requireOptional("ws");

function getEnv(name, fallback = "") {
  const value = process.env[name];
  return typeof value === "string" && value.trim().length > 0 ? value.trim() : fallback;
}

function getEnvInt(name, fallback) {
  const raw = process.env[name];
  if (typeof raw !== "string" || raw.trim().length === 0) {
    return fallback;
  }
  const value = Number.parseInt(raw, 10);
  return Number.isFinite(value) ? value : fallback;
}

function getEnvBool(name, fallback) {
  const raw = process.env[name];
  if (typeof raw !== "string" || raw.trim().length === 0) {
    return fallback;
  }
  const normalized = raw.trim().toLowerCase();
  if (["1", "true", "yes", "on"].includes(normalized)) return true;
  if (["0", "false", "no", "off"].includes(normalized)) return false;
  return fallback;
}

function parseCsvEnv(name, fallback = []) {
  const raw = process.env[name];
  if (typeof raw !== "string" || raw.trim().length === 0) {
    return fallback;
  }
  return raw.split(",").map((v) => v.trim()).filter(Boolean);
}

function nowIso() {
  return new Date().toISOString();
}

function log(level, message, details) {
  const payload = {
    ts: nowIso(),
    level,
    message,
    ...(details && typeof details === "object" ? details : {}),
  };
  const line = JSON.stringify(payload);
  if (level === "error") console.error(line);
  else console.log(line);
}

function normalizeTopicPart(value, fallback) {
  const normalized = (value || "")
    .toString()
    .trim()
    .replaceAll(/[^a-zA-Z0-9._-]/g, "-")
    .replaceAll(/-+/g, "-")
    .replace(/^-+|-+$/g, "");
  return normalized.length > 0 ? normalized : fallback;
}

function normalizeStatus(value) {
  if (typeof value !== "string") return "";
  const normalized = value.trim().toLowerCase();
  if (LEGACY_WORKING_STATUS_VALUES.has(normalized)) return "working";
  return STATUS_VALUES.has(normalized) ? normalized : "";
}

function normalizeAgentId(value) {
  if (typeof value !== "string" || !value.trim()) return "claude-code";
  return normalizeTopicPart(value.trim().toLowerCase(), "claude-code");
}

function mapClawdStateToStatus(state, event) {
  const safe = typeof state === "string" ? state.trim().toLowerCase() : "";
  const hookEvent = typeof event === "string" ? event.trim() : "";

  if (hookEvent === "AssistantMessage" || hookEvent === "AssistantOutput") return "speaking";
  if (hookEvent === "PermissionRequest" || hookEvent === "Elicitation") return "waiting_user";
  if (safe === "codex-permission") return "waiting_user";
  if (safe === "notification") return "waiting_user";
  if (safe === "error") return "error";
  if (safe === "speaking") return "speaking";
  if (safe === "attention") return "done";

  if (
    [
      "working",
      "tool_running",
      "thinking",
      "juggling",
      "sweeping",
      "carrying",
      "waking",
      "mini-enter",
      "mini-crabwalk",
    ].includes(safe)
  ) return "working";

  if (
    [
      "idle",
      "sleeping",
      "yawning",
      "dozing",
      "collapsing",
      "mini-idle",
      "mini-sleep",
      "mini-enter-sleep",
      "mini-peek",
    ].includes(safe)
  ) {
    return "idle";
  }

  if (safe === "mini-alert") return "waiting_user";
  if (safe === "mini-happy") return "done";

  return "";
}

function toLowerAscii(input) {
  return input.replace(/[A-Z]/g, (char) => String.fromCharCode(char.charCodeAt(0) + 32));
}

function normalizeDeviceMetadataForAuth(value) {
  if (typeof value !== "string") return "";
  const trimmed = value.trim();
  if (!trimmed) return "";
  return toLowerAscii(trimmed);
}

function base64UrlEncode(buffer) {
  return buffer.toString("base64").replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/g, "");
}

function derivePublicKeyRaw(publicKeyPem) {
  const key = crypto.createPublicKey(publicKeyPem);
  const spki = key.export({ type: "spki", format: "der" });
  if (
    spki.length === ED25519_SPKI_PREFIX.length + 32
    && spki.subarray(0, ED25519_SPKI_PREFIX.length).equals(ED25519_SPKI_PREFIX)
  ) {
    return spki.subarray(ED25519_SPKI_PREFIX.length);
  }
  return spki;
}

function fingerprintPublicKey(publicKeyPem) {
  return crypto.createHash("sha256").update(derivePublicKeyRaw(publicKeyPem)).digest("hex");
}

function publicKeyRawBase64UrlFromPem(publicKeyPem) {
  return base64UrlEncode(derivePublicKeyRaw(publicKeyPem));
}

function signDevicePayload(privateKeyPem, payload) {
  const key = crypto.createPrivateKey(privateKeyPem);
  const signature = crypto.sign(null, Buffer.from(payload, "utf8"), key);
  return base64UrlEncode(signature);
}

function buildDeviceAuthPayloadV3(params) {
  const scopes = Array.isArray(params.scopes) ? params.scopes.join(",") : "";
  const token = params.token ?? "";
  const platform = normalizeDeviceMetadataForAuth(params.platform);
  const deviceFamily = normalizeDeviceMetadataForAuth(params.deviceFamily);
  return [
    "v3",
    params.deviceId,
    params.clientId,
    params.clientMode,
    params.role,
    scopes,
    String(params.signedAtMs),
    token,
    params.nonce,
    platform,
    deviceFamily,
  ].join("|");
}

function ensureParentDir(filePath) {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
}

function loadOrCreateDeviceIdentity(identityPath) {
  try {
    if (fs.existsSync(identityPath)) {
      const raw = fs.readFileSync(identityPath, "utf8");
      const parsed = JSON.parse(raw);
      if (
        parsed
        && parsed.version === 1
        && typeof parsed.deviceId === "string"
        && typeof parsed.publicKeyPem === "string"
        && typeof parsed.privateKeyPem === "string"
      ) {
        const derivedId = fingerprintPublicKey(parsed.publicKeyPem);
        if (derivedId !== parsed.deviceId) {
          const next = { ...parsed, deviceId: derivedId };
          fs.writeFileSync(identityPath, `${JSON.stringify(next, null, 2)}\n`, { mode: 0o600 });
          try { fs.chmodSync(identityPath, 0o600); } catch {}
        }
        return {
          deviceId: derivedId,
          publicKeyPem: parsed.publicKeyPem,
          privateKeyPem: parsed.privateKeyPem,
        };
      }
    }
  } catch {}

  const keypair = crypto.generateKeyPairSync("ed25519");
  const publicKeyPem = keypair.publicKey.export({ type: "spki", format: "pem" }).toString();
  const privateKeyPem = keypair.privateKey.export({ type: "pkcs8", format: "pem" }).toString();
  const deviceId = fingerprintPublicKey(publicKeyPem);
  const stored = {
    version: 1,
    deviceId,
    publicKeyPem,
    privateKeyPem,
    createdAtMs: Date.now(),
  };
  ensureParentDir(identityPath);
  fs.writeFileSync(identityPath, `${JSON.stringify(stored, null, 2)}\n`, { mode: 0o600 });
  try { fs.chmodSync(identityPath, 0o600); } catch {}
  return { deviceId, publicKeyPem, privateKeyPem };
}

function pickToolName(data) {
  if (!data || typeof data !== "object") return null;
  const candidates = [
    data.tool,
    data.toolName,
    data.name,
    data.command,
    data.call && typeof data.call === "object" ? data.call.name : null,
  ];
  for (const value of candidates) {
    if (typeof value === "string" && value.trim().length > 0) {
      return value.trim();
    }
  }
  return null;
}

function readFiniteNumber(...values) {
  for (const value of values) {
    if (typeof value !== "number") continue;
    if (Number.isFinite(value)) return value;
  }
  return undefined;
}

function normalizeTokenUsage(raw) {
  if (!raw || typeof raw !== "object") return null;

  const out = {
    inputTokens: readFiniteNumber(raw.inputTokens, raw.input_tokens, raw.promptTokens, raw.prompt_tokens),
    outputTokens: readFiniteNumber(
      raw.outputTokens,
      raw.output_tokens,
      raw.completionTokens,
      raw.completion_tokens
    ),
    cachedInputTokens: readFiniteNumber(
      raw.cachedInputTokens,
      raw.cached_input_tokens,
      raw.cacheReadInputTokens,
      raw.cache_read_input_tokens
    ),
    cacheCreationInputTokens: readFiniteNumber(
      raw.cacheCreationInputTokens,
      raw.cache_creation_input_tokens
    ),
    reasoningOutputTokens: readFiniteNumber(
      raw.reasoningOutputTokens,
      raw.reasoning_output_tokens,
      raw.reasoningTokens,
      raw.reasoning_tokens
    ),
    totalTokens: readFiniteNumber(raw.totalTokens, raw.total_tokens),
    contextTokens: readFiniteNumber(raw.contextTokens, raw.context_tokens),
    estimatedCostUsd: readFiniteNumber(raw.estimatedCostUsd, raw.estimated_cost_usd, raw.costUsd, raw.cost_usd),
  };

  if (out.totalTokens == null) {
    const derived = (out.inputTokens || 0)
      + (out.cachedInputTokens || 0)
      + (out.cacheCreationInputTokens || 0)
      + (out.outputTokens || 0);
    if (derived > 0) out.totalTokens = derived;
  }

  const hasAny = Object.values(out).some((value) => Number.isFinite(value));
  return hasAny ? out : null;
}

function extractTokenUsageFromPayload(payload) {
  if (!payload || typeof payload !== "object") return null;
  const objects = [
    payload,
    payload.token_usage,
    payload.usage,
    payload.metrics,
    payload.session,
    payload.snapshot,
    payload.data,
    payload.message,
    payload.data && typeof payload.data === "object" ? payload.data.usage : null,
    payload.session && typeof payload.session === "object" ? payload.session.usage : null,
  ];

  for (const candidate of objects) {
    const usage = normalizeTokenUsage(candidate);
    if (usage) return usage;
  }
  return null;
}

function mapAgentEvent(payload) {
  if (!payload || typeof payload !== "object") return null;
  const stream = typeof payload.stream === "string" ? payload.stream : "";
  const data = payload.data && typeof payload.data === "object" ? payload.data : {};
  const phase = typeof data.phase === "string" ? data.phase : "";
  const runId = typeof payload.runId === "string" ? payload.runId : null;
  const sessionKey = typeof payload.sessionKey === "string" ? payload.sessionKey : null;
  const tokenUsage = extractTokenUsageFromPayload(payload);

  if (stream === "assistant") {
    return { state: "speaking", runId, sessionKey, reason: "agent.assistant", tokenUsage };
  }

  if (stream === "tool") {
    const toolName = pickToolName(data);
    if (phase === "error") {
      return {
        state: "error",
        runId,
        sessionKey,
        reason: "agent.tool.error",
        detail: toolName ? { tool: toolName } : undefined,
        tokenUsage,
      };
    }
    if (phase === "end") {
      return {
        state: "working",
        runId,
        sessionKey,
        reason: "agent.tool.end",
        detail: toolName ? { tool: toolName } : undefined,
        tokenUsage,
      };
    }
    return {
      state: "working",
      runId,
      sessionKey,
      reason: "agent.tool",
      detail: toolName ? { tool: toolName } : undefined,
      tokenUsage,
    };
  }

  if (stream === "lifecycle") {
    if (phase === "start") {
      return { state: "working", runId, sessionKey, reason: "agent.lifecycle.start", tokenUsage };
    }
    if (phase === "end") {
      return { state: "done", runId, sessionKey, reason: "agent.lifecycle.end", tokenUsage };
    }
    if (phase === "error") {
      return { state: "error", runId, sessionKey, reason: "agent.lifecycle.error", tokenUsage };
    }
  }

  if (stream === "error") {
    return { state: "error", runId, sessionKey, reason: "agent.error", tokenUsage };
  }

  return null;
}

function mapSessionToolEvent(payload) {
  if (!payload || typeof payload !== "object") return null;
  const data = payload.data && typeof payload.data === "object" ? payload.data : {};
  const phase = typeof data.phase === "string" ? data.phase : "";
  const runId = typeof payload.runId === "string" ? payload.runId : null;
  const sessionKey = typeof payload.sessionKey === "string" ? payload.sessionKey : null;
  const toolName = pickToolName(data);
  const tokenUsage = extractTokenUsageFromPayload(payload);

  if (phase === "error") {
    return {
      state: "error",
      runId,
      sessionKey,
      reason: "session.tool.error",
      detail: toolName ? { tool: toolName } : undefined,
      tokenUsage,
    };
  }
  if (phase === "end") {
    return {
      state: "working",
      runId,
      sessionKey,
      reason: "session.tool.end",
      detail: toolName ? { tool: toolName } : undefined,
      tokenUsage,
    };
  }
  return {
    state: "working",
    runId,
    sessionKey,
    reason: "session.tool",
    detail: toolName ? { tool: toolName } : undefined,
    tokenUsage,
  };
}

function mapChatEvent(payload) {
  if (!payload || typeof payload !== "object") return null;
  const state = typeof payload.state === "string" ? payload.state : "";
  const runId = typeof payload.runId === "string" ? payload.runId : null;
  const sessionKey = typeof payload.sessionKey === "string" ? payload.sessionKey : null;
  const tokenUsage = extractTokenUsageFromPayload(payload);

  if (state === "delta") {
    return { state: "speaking", runId, sessionKey, reason: "chat.delta", tokenUsage };
  }
  if (state === "final") {
    return { state: "done", runId, sessionKey, reason: "chat.final", tokenUsage };
  }
  if (state === "aborted") {
    return { state: "waiting_user", runId, sessionKey, reason: "chat.aborted", tokenUsage };
  }
  if (state === "error") {
    return { state: "error", runId, sessionKey, reason: "chat.error", tokenUsage };
  }
  return null;
}

function mapSessionsChangedEvent(payload) {
  if (!payload || typeof payload !== "object") return null;
  const phase = typeof payload.phase === "string" ? payload.phase : "";
  const runId = typeof payload.runId === "string" ? payload.runId : null;
  const sessionKey = typeof payload.sessionKey === "string" ? payload.sessionKey : null;
  const tokenUsage = extractTokenUsageFromPayload(payload);

  if (phase === "start" || phase === "send" || phase === "steer") {
    return { state: "working", runId, sessionKey, reason: `sessions.changed.${phase}`, tokenUsage };
  }
  if (phase === "end") {
    return { state: "done", runId, sessionKey, reason: "sessions.changed.end", tokenUsage };
  }
  if (phase === "error") {
    return { state: "error", runId, sessionKey, reason: "sessions.changed.error", tokenUsage };
  }
  return null;
}

function mapGatewayEvent(event, payload) {
  if (event === "agent") return mapAgentEvent(payload);
  if (event === "session.tool") return mapSessionToolEvent(payload);
  if (event === "chat") return mapChatEvent(payload);
  if (event === "sessions.changed") return mapSessionsChangedEvent(payload);
  return null;
}

function enrichPayloadMetrics(metricsTracker, payload) {
  if (!metricsTracker || !payload || typeof payload !== "object") return payload;
  const nowMs = Number.isFinite(payload.tsMs) ? payload.tsMs : Date.now();
  return metricsTracker.apply(payload, nowMs);
}

class MqttPublisher {
  constructor(config) {
    this.config = config;
    this.client = null;
    this.connected = false;
    this.pendingByTopic = new Map();
    this.lastByTopic = new Map();
    this.deviceAvailability = new Map();

    const namespace = normalizeTopicPart(config.namespace, "desk");
    const deviceId = normalizeTopicPart(config.deviceId, "devbox");
    this.baseTopic = `${namespace}/${deviceId}`;
    this.topicActive = `${this.baseTopic}/state/${normalizeTopicPart(config.activeTopicSuffix, "active")}`;
    this.topicSpeech = `${this.baseTopic}/speech/text`;
    this.topicBridgeAvailability = `${this.baseTopic}/availability/bridge`;
    this.boardAvailabilityPattern = "claw-pet/board/+/availability";
    this.boardInputActionPattern = "claw-pet/board/+/input/action";
    this.onBoardInputAction = typeof config.onBoardInputAction === "function"
      ? config.onBoardInputAction
      : null;
    this.enableActiveTopic = config.enableActiveTopic === true;
    this.enabledSources = new Set(
      (Array.isArray(config.enabledSources) ? config.enabledSources : [])
        .map((source) => normalizeAgentId(source))
        .filter(Boolean)
    );
    this.selectedSource = typeof config.selectedSource === "string" && config.selectedSource.trim()
      ? normalizeAgentId(config.selectedSource)
      : "";
    this.localStateDir = typeof config.localStateDir === "string" && config.localStateDir.trim()
      ? config.localStateDir.trim()
      : path.join(os.tmpdir(), "pet-manager-bridge-state");
  }

  start() {
    const connectOptions = {
      clientId: this.config.clientId,
      clean: true,
      reconnectPeriod: Math.max(500, this.config.reconnectMs),
      username: this.config.username || undefined,
      password: this.config.password || undefined,
      will: {
        topic: this.topicBridgeAvailability,
        payload: JSON.stringify({ source: "bridge", online: false, ts: nowIso() }),
        qos: this.config.qos,
        retain: true,
      },
    };

    this.client = mqtt.connect(this.config.url, connectOptions);

    this.client.on("connect", () => {
      this.connected = true;
      log("info", "mqtt connected", {
        broker: this.config.url,
        baseTopic: this.baseTopic,
        activeTopic: this.enableActiveTopic ? this.topicActive : undefined,
      });
      this.publishAvailability("bridge", true);
      this.clearLegacyActiveTopic();
      this.clearDisabledSourceTopics();
      this.flushPending();

      this.client.subscribe(this.boardAvailabilityPattern, { qos: 0 }, (err) => {
        if (err) {
          log("warn", "failed to subscribe to board availability", { error: String(err) });
        } else {
          log("info", "subscribed to board availability", { pattern: this.boardAvailabilityPattern });
        }
      });
      this.client.subscribe(this.boardInputActionPattern, { qos: 0 }, (err) => {
        if (err) {
          log("warn", "failed to subscribe to board input action", { error: String(err) });
        } else {
          log("info", "subscribed to board input action", { pattern: this.boardInputActionPattern });
        }
      });
    });

    this.client.on("reconnect", () => {
      log("info", "mqtt reconnecting", { broker: this.config.url });
    });

    this.client.on("close", () => {
      this.connected = false;
      log("warn", "mqtt disconnected", { broker: this.config.url });
    });

    this.client.on("error", (error) => {
      log("error", "mqtt error", { error: String(error) });
    });

    this.client.on("message", (topic, message) => {
      this._handleMessage(topic, message);
    });
  }

  _handleMessage(topic, message) {
    // Match claw-pet/board/<deviceId>/availability
    const availabilityMatch = topic.match(/^claw-pet\/board\/([^/]+)\/availability$/);
    if (availabilityMatch) {
      const boardDeviceId = availabilityMatch[1];
      try {
        const text = message.toString();
        if (!text.trim()) {
          this.deviceAvailability.delete(boardDeviceId);
          return;
        }
        const payload = JSON.parse(text);
        const targetDeviceId = normalizeTopicPart(payload.targetDeviceId || payload.desktopDeviceId || "", "");
        const mqttNamespace = normalizeTopicPart(payload.mqttNamespace || this.config.namespace || "", "");
        const targetSource = payload.targetSource ? normalizeAgentId(payload.targetSource) : "";
        this.deviceAvailability.set(boardDeviceId, {
          online: Boolean(payload.online),
          ts: payload.ts || nowIso(),
          receivedAt: nowIso(),
          boardDeviceId: payload.boardDeviceId || boardDeviceId,
          localDeviceId: payload.localDeviceId || boardDeviceId,
          desktopDeviceId: targetDeviceId,
          targetDeviceId,
          targetSource,
          mqttNamespace,
          name: payload.name || "",
          model: payload.model || "",
        });
        log("info", "board availability update", {
          boardDeviceId,
          online: payload.online,
          targetDeviceId,
          targetSource,
        });
      } catch (err) {
        log("warn", "failed to parse board availability message", { topic, error: String(err) });
      }
      return;
    }

    const inputActionMatch = topic.match(/^claw-pet\/board\/([^/]+)\/input\/action$/);
    if (inputActionMatch) {
      if (!this.onBoardInputAction) return;
      const boardDeviceId = normalizeTopicPart(inputActionMatch[1], "");
      try {
        const text = message.toString();
        if (!text.trim()) return;
        const payload = JSON.parse(text);
        Promise.resolve(this.onBoardInputAction({
          topic,
          boardDeviceId,
          payload,
          receivedAt: nowIso(),
        })).catch((err) => {
          log("warn", "board input action handler failed", {
            topic,
            boardDeviceId,
            error: String(err),
          });
        });
      } catch (err) {
        log("warn", "failed to parse board input action message", {
          topic,
          boardDeviceId,
          error: String(err),
        });
      }
      return;
    }
  }

  getBoardAvailabilityStatus(boardDeviceId) {
    const normalized = normalizeTopicPart(boardDeviceId || "", "");
    if (!normalized) return null;
    const direct = this.deviceAvailability.get(normalized);
    if (direct) return direct;
    for (const status of this.deviceAvailability.values()) {
      const candidate = normalizeTopicPart((status && status.boardDeviceId) || "", "");
      if (candidate && candidate === normalized) {
        return status;
      }
    }
    return null;
  }

  getDeviceAvailability() {
    const result = {};
    for (const [id, status] of this.deviceAvailability.entries()) {
      result[id] = status;
    }
    return result;
  }

  getPreferredSource() {
    if (this.selectedSource) return this.selectedSource;
    for (const source of this.enabledSources.values()) {
      if (source) return source;
    }
    return "";
  }

  resolveRemoteBindingBoardIds({ boardDeviceId, targetDeviceId, mqttNamespace }) {
    return this.resolveRemoteBindingTargets({ boardDeviceId, targetDeviceId, mqttNamespace })
      .map((target) => target.boardDeviceId);
  }

  resolveRemoteBindingTargets({ boardDeviceId, targetDeviceId, mqttNamespace }) {
    const ids = [];
    const targets = [];
    const add = (value) => {
      const id = normalizeTopicPart(value || "", "");
      if (!id || ids.includes(id)) return;
      ids.push(id);
      targets.push({
        boardDeviceId: id,
        localDeviceId: id,
        controlTopic: `${normalizeTopicPart(mqttNamespace || this.config.namespace || "desk", "desk")}/${id}/control/remote-cli-binding`,
      });
    };
    const addStatus = (id, status) => {
      const boardId = normalizeTopicPart((status && status.boardDeviceId) || id || "", "");
      const localId = normalizeTopicPart((status && status.localDeviceId) || boardId || "", "");
      if (!boardId || ids.includes(boardId)) return;
      ids.push(boardId);
      targets.push({
        boardDeviceId: boardId,
        localDeviceId: localId,
        controlTopic: `${normalizeTopicPart(mqttNamespace || this.config.namespace || "desk", "desk")}/${localId}/control/remote-cli-binding`,
      });
    };
    const target = normalizeTopicPart(targetDeviceId || "", "");
    const namespace = normalizeTopicPart(mqttNamespace || this.config.namespace || "", "");
    const onlineFallbackIds = [];

    add(boardDeviceId);

    for (const [id, status] of this.deviceAvailability.entries()) {
      if (!status || status.online !== true) continue;
      const onlineId = normalizeTopicPart(status.boardDeviceId || id, "");
      if (onlineId) onlineFallbackIds.push(onlineId);
      const statusTarget = normalizeTopicPart(
        status.targetDeviceId || status.desktopDeviceId || "",
        "",
      );
      const statusNamespace = normalizeTopicPart(status.mqttNamespace || "", "");
      if (!target || statusTarget !== target) continue;
      if (statusNamespace && namespace && statusNamespace !== namespace) continue;
      addStatus(id, status);
    }

    if (ids.length <= (boardDeviceId ? 1 : 0) && onlineFallbackIds.length === 1) {
      const fallbackId = onlineFallbackIds[0];
      addStatus(fallbackId, this.deviceAvailability.get(fallbackId));
    }

    return targets;
  }

  sourceTopic(sourceId) {
    return `${this.baseTopic}/state/${normalizeTopicPart(sourceId, "unknown")}`;
  }

  availabilityTopic(sourceId) {
    return `${this.baseTopic}/availability/${normalizeTopicPart(sourceId, "unknown")}`;
  }

  flushPending() {
    for (const [topic, packet] of this.pendingByTopic.entries()) {
      this._publish(topic, packet.payloadText, packet.options, true);
      this.pendingByTopic.delete(topic);
    }
  }

  _publish(topic, payloadText, options, force = false) {
    if (!this.client || !this.connected) {
      this.pendingByTopic.set(topic, { payloadText, options });
      return;
    }

    if (!force) {
      const last = this.lastByTopic.get(topic);
      if (last === payloadText) return;
    }

    this.lastByTopic.set(topic, payloadText);
    this.client.publish(topic, payloadText, options);
  }

  publishJson(topic, payload, options = {}) {
    const payloadText = JSON.stringify(payload);
    this._publish(topic, payloadText, {
      qos: this.config.qos,
      retain: Boolean(options.retain),
    });
  }

  publishSource(payload) {
    const source = normalizeAgentId(payload.source || "unknown");
    this.publishJson(this.sourceTopic(source), payload, { retain: Boolean(this.config.retain) });
    // Write latest state to local file for USB serial bridge to poll
    this._writeLocalState(source, payload);
  }

  publishSpeech(sourceId, payload) {
    const source = normalizeAgentId(sourceId || payload?.source || "unknown");
    if (this.selectedSource && source !== this.selectedSource) return;
    const speechPayload = {
      source,
      ...(payload && typeof payload === "object" ? payload : {}),
    };
    this.publishJson(this.topicSpeech, speechPayload, { retain: false });
    this._writeLocalSpeech(source, speechPayload);
  }

  _writeLocalState(source, payload) {
    try {
      fs.mkdirSync(this.localStateDir, { recursive: true });
      const filePath = path.join(this.localStateDir, `${source}.json`);
      fs.writeFileSync(filePath, JSON.stringify(payload) + "\n", "utf8");
    } catch (e) {
      // best-effort, don't break MQTT flow
    }
  }

  _writeLocalSpeech(source, payload) {
    try {
      if (!this._localSpeechDir) {
        const os = require("os");
        const path = require("path");
        this._localSpeechDir = path.join(os.tmpdir(), "pet-manager-bridge-speech");
        require("fs").mkdirSync(this._localSpeechDir, { recursive: true });
      }
      const path = require("path");
      const fs = require("fs");
      const filePath = path.join(this._localSpeechDir, `${source}.json`);
      fs.writeFileSync(filePath, JSON.stringify(payload) + "\n", "utf8");
    } catch (e) {
      // best-effort, don't break MQTT flow
    }
  }

  publishActive(payload) {
    if (!this.enableActiveTopic) return;
    this.publishJson(this.topicActive, payload, { retain: Boolean(this.config.retain) });
  }

  clearLegacyActiveTopic() {
    if (!this.client || !this.connected) return;
    this.client.publish(this.topicActive, "", { qos: this.config.qos, retain: true });
    this.lastByTopic.delete(this.topicActive);
  }

  clearDisabledSourceTopics() {
    if (!this.client || !this.connected || this.enabledSources.size === 0) return;
    for (const source of KNOWN_AGENT_SOURCES) {
      if (this.enabledSources.has(source)) continue;
      const topic = this.sourceTopic(source);
      this.client.publish(topic, "", { qos: this.config.qos, retain: true });
      this.lastByTopic.delete(topic);
    }
  }

  publishAvailability(sourceId, online) {
    const payload = {
      source: normalizeAgentId(sourceId),
      online: Boolean(online),
      ts: nowIso(),
      tsMs: Date.now(),
    };
    this.publishJson(this.availabilityTopic(sourceId), payload, { retain: true });
  }

  stop() {
    if (!this.client) return;
    try {
      if (this.connected) this.publishAvailability("bridge", false);
      this.client.end(true);
    } catch {}
    this.client = null;
    this.connected = false;
  }
}

class OpenClawStatusController {
  constructor(config) {
    this.config = config;
    this.publisher = config.publisher;
    this.includeRaw = config.includeRaw;
    this.metricsTracker = config.metricsTracker || null;

    this.state = "idle";
    this.reason = "startup";
    this.runId = null;
    this.sessionKey = null;
    this.detail = undefined;
    this.tokenUsage = undefined;
    this.raw = undefined;
    this.gatewayConnected = false;
    this.lastFingerprint = "";
    this.lastActivityAt = 0;

    this.idleTimer = null;
    this.heartbeatTimer = null;
  }

  start() {
    this.startIdleWatch();
    this.startHeartbeat();
    this.publishCurrent(true, { reason: "startup" });
  }

  stop() {
    if (this.idleTimer) clearInterval(this.idleTimer);
    if (this.heartbeatTimer) clearInterval(this.heartbeatTimer);
    this.idleTimer = null;
    this.heartbeatTimer = null;
  }

  setGatewayConnected(connected) {
    if (this.gatewayConnected === connected) return;
    this.gatewayConnected = connected;
    this.publisher.publishAvailability("openclaw", connected);
    if (!connected) {
      this.transition({ state: "idle", reason: "gateway.disconnected" });
      return;
    }
    this.publishCurrent(true, { reason: "gateway.connected" });
  }

  consumeEvent(event, payload) {
    const mapped = mapGatewayEvent(event, payload);
    if (!mapped) return;
    this.transition({
      ...mapped,
      raw: this.includeRaw ? payload : undefined,
    });
  }

  transition(update) {
    const nextState = normalizeStatus(update.state) || this.state;
    this.lastActivityAt = Date.now();
    this.state = nextState;
    this.reason = typeof update.reason === "string" && update.reason ? update.reason : this.reason;
    this.runId = typeof update.runId === "string" && update.runId ? update.runId : null;
    this.sessionKey = typeof update.sessionKey === "string" && update.sessionKey ? update.sessionKey : null;
    this.detail = update.detail && typeof update.detail === "object" ? update.detail : undefined;
    if (update.tokenUsage && typeof update.tokenUsage === "object") {
      this.tokenUsage = update.tokenUsage;
    }
    this.raw = update.raw;

    this.publishCurrent(false);
  }

  startIdleWatch() {
    if (this.idleTimer) clearInterval(this.idleTimer);
    const tickMs = Math.max(500, Math.floor(this.config.idleWatchTickMs));
    this.idleTimer = setInterval(() => {
      if (!this.lastActivityAt) return;
      if (this.state === "idle" || this.state === "done" || this.state === "error") return;
      const idleForMs = Date.now() - this.lastActivityAt;
      if (idleForMs < this.config.idleTimeoutMs) return;
      this.transition({ state: "idle", reason: "idle.timeout" });
    }, tickMs);
    this.idleTimer.unref?.();
  }

  startHeartbeat() {
    if (this.heartbeatTimer) clearInterval(this.heartbeatTimer);
    if (!Number.isFinite(this.config.heartbeatMs) || this.config.heartbeatMs <= 0) return;
    this.heartbeatTimer = setInterval(() => {
      if (this.state === "done" || this.state === "error") return;
      this.publishCurrent(true, { reason: "heartbeat", silent: true });
    }, this.config.heartbeatMs);
    this.heartbeatTimer.unref?.();
  }

  publishCurrent(force = false, opts = {}) {
    const fingerprintObj = {
      state: this.state,
      reason: this.reason,
      runId: this.runId,
      sessionKey: this.sessionKey,
      gatewayConnected: this.gatewayConnected,
      detail: this.detail,
      tokenUsage: this.tokenUsage,
    };
    const fingerprint = JSON.stringify(fingerprintObj);
    if (!force && fingerprint === this.lastFingerprint) return;
    this.lastFingerprint = fingerprint;

    const payload = {
      source: "openclaw",
      channel: "openclaw-gateway",
      bridge: APP_NAME,
      bridgeVersion: APP_VERSION,
      state: this.state,
      reason: this.reason,
      runId: this.runId,
      sessionKey: this.sessionKey,
      gatewayConnected: this.gatewayConnected,
      detail: this.detail,
      tokenUsage: this.tokenUsage,
      ts: nowIso(),
      tsMs: Date.now(),
      ...(this.includeRaw && this.raw ? { raw: this.raw } : {}),
    };

    const enriched = enrichPayloadMetrics(this.metricsTracker, payload);
    this.publisher.publishSource(enriched);

    if (!opts.silent) {
      log("info", "openclaw status published", {
        state: enriched.state,
        reason: enriched.reason,
        runId: enriched.runId,
        sessionKey: enriched.sessionKey,
      });
    }
  }
}

class OpenClawGatewayBridge {
  constructor(config) {
    this.config = config;
    this.ws = null;
    this.closed = false;
    this.backoffMs = Math.max(500, config.reconnectMinMs);
    this.reconnectTimer = null;
    this.challengeTimer = null;
    this.pending = new Map();
    this.deviceIdentity = null;

    if (this.config.enableDeviceAuth) {
      try {
        this.deviceIdentity = loadOrCreateDeviceIdentity(this.config.deviceIdentityPath);
        log("info", "openclaw device identity ready", {
          deviceId: this.deviceIdentity.deviceId,
          identityPath: this.config.deviceIdentityPath,
        });
      } catch (error) {
        this.deviceIdentity = null;
        log("warn", "failed to initialize openclaw device identity", {
          error: String(error),
          identityPath: this.config.deviceIdentityPath,
        });
      }
    }
  }

  start() {
    this.closed = false;
    this.connect();
  }

  stop() {
    this.closed = true;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    if (this.challengeTimer) clearTimeout(this.challengeTimer);
    this.reconnectTimer = null;
    this.challengeTimer = null;

    if (this.ws) {
      try { this.ws.close(); } catch {}
    }
    this.flushPending(new Error("bridge stopped"));
  }

  connect() {
    if (this.closed) return;

    const ws = new WebSocket(this.config.gatewayUrl, {
      maxPayload: 25 * 1024 * 1024,
    });
    this.ws = ws;

    ws.on("open", () => {
      if (this.ws !== ws) return;
      log("info", "openclaw socket opened", { url: this.config.gatewayUrl });
      this.config.onConnectionState(true);
      this.scheduleChallengeTimeout();
    });

    ws.on("message", (raw) => {
      if (this.ws !== ws) return;
      this.handleRawMessage(raw.toString());
    });

    ws.on("close", (code, reasonBuffer) => {
      if (this.ws === ws) this.ws = null;
      const reason = Buffer.isBuffer(reasonBuffer) ? reasonBuffer.toString("utf8") : String(reasonBuffer || "");
      log("warn", "openclaw socket closed", { code, reason });
      this.config.onConnectionState(false);
      this.flushPending(new Error(`gateway closed (${code}) ${reason}`));
      this.clearChallengeTimer();
      this.scheduleReconnect();
    });

    ws.on("error", (error) => {
      log("error", "openclaw socket error", { error: String(error) });
    });
  }

  scheduleChallengeTimeout() {
    this.clearChallengeTimer();
    this.challengeTimer = setTimeout(() => {
      this.challengeTimer = null;
      const hasPendingConnect = Array.from(this.pending.values()).some((entry) => entry.method === "connect");
      if (hasPendingConnect) return;
      log("error", "connect.challenge timeout", { timeoutMs: this.config.challengeTimeoutMs });
      if (this.ws) this.ws.close(1008, "connect challenge timeout");
    }, this.config.challengeTimeoutMs);
    this.challengeTimer.unref?.();
  }

  clearChallengeTimer() {
    if (!this.challengeTimer) return;
    clearTimeout(this.challengeTimer);
    this.challengeTimer = null;
  }

  async handleConnectChallenge(payload) {
    const nonce = payload && typeof payload.nonce === "string" ? payload.nonce.trim() : "";
    if (!nonce) {
      log("error", "connect.challenge missing nonce");
      if (this.ws) this.ws.close(1008, "connect.challenge missing nonce");
      return;
    }

    const auth = {};
    if (this.config.token) auth.token = this.config.token;
    if (this.config.password) auth.password = this.config.password;
    if (this.config.deviceToken) auth.deviceToken = this.config.deviceToken;

    let device;
    if (this.deviceIdentity) {
      const signedAtMs = Date.now();
      const signatureToken = this.config.token || this.config.deviceToken || "";
      const signedPayload = buildDeviceAuthPayloadV3({
        deviceId: this.deviceIdentity.deviceId,
        clientId: this.config.clientId,
        clientMode: this.config.clientMode,
        role: "operator",
        scopes: this.config.scopes,
        signedAtMs,
        token: signatureToken,
        nonce,
        platform: this.config.platform,
        deviceFamily: this.config.deviceFamily,
      });
      device = {
        id: this.deviceIdentity.deviceId,
        publicKey: publicKeyRawBase64UrlFromPem(this.deviceIdentity.publicKeyPem),
        signature: signDevicePayload(this.deviceIdentity.privateKeyPem, signedPayload),
        signedAt: signedAtMs,
        nonce,
      };
    }

    const params = {
      minProtocol: this.config.protocolVersion,
      maxProtocol: this.config.protocolVersion,
      client: {
        id: this.config.clientId,
        version: APP_VERSION,
        platform: this.config.platform,
        deviceFamily: this.config.deviceFamily,
        mode: this.config.clientMode,
        instanceId: this.config.instanceId,
      },
      role: "operator",
      scopes: this.config.scopes,
      caps: [],
      commands: [],
      permissions: {},
      auth: Object.keys(auth).length > 0 ? auth : undefined,
      device,
      userAgent: `${APP_NAME}/${APP_VERSION}`,
      locale: this.config.locale,
    };

    try {
      const payloadRes = await this.sendRequest("connect", params, this.config.connectTimeoutMs);
      if (!payloadRes || payloadRes.type !== "hello-ok") {
        throw new Error("gateway connect did not return hello-ok payload");
      }
      this.clearChallengeTimer();
      this.backoffMs = this.config.reconnectMinMs;
      log("info", "openclaw handshake complete", {
        protocol: payloadRes.protocol,
        scopes: this.config.scopes,
      });
      await this.subscribeEvents();
    } catch (error) {
      log("error", "openclaw connect failed", { error: String(error) });
      if (this.ws) this.ws.close(1008, "connect failed");
    }
  }

  async subscribeEvents() {
    if (!this.config.subscribeSessions) return;
    try {
      const result = await this.sendRequest("sessions.subscribe", undefined, this.config.requestTimeoutMs);
      log("info", "openclaw sessions.subscribe complete", { result });
    } catch (error) {
      log("warn", "openclaw sessions.subscribe failed", { error: String(error) });
    }
  }

  sendRequest(method, params, timeoutMs) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      throw new Error(`socket not open for method ${method}`);
    }

    const id = crypto.randomUUID();
    const frame = {
      type: "req",
      id,
      method,
      ...(params !== undefined ? { params } : {}),
    };

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`request timeout: ${method}`));
      }, timeoutMs);
      timer.unref?.();

      this.pending.set(id, { method, resolve, reject, timer });

      try {
        this.ws.send(JSON.stringify(frame));
      } catch (error) {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(error instanceof Error ? error : new Error(String(error)));
      }
    });
  }

  handleRawMessage(raw) {
    let frame;
    try {
      frame = JSON.parse(raw);
    } catch (error) {
      log("warn", "discard invalid openclaw frame", { error: String(error) });
      return;
    }

    if (!frame || typeof frame !== "object") return;

    if (frame.type === "event") {
      const event = typeof frame.event === "string" ? frame.event : "";
      if (!event) return;
      if (event === "connect.challenge") {
        void this.handleConnectChallenge(frame.payload);
        return;
      }
      this.config.onEvent(event, frame.payload);
      return;
    }

    if (frame.type === "res") {
      const id = typeof frame.id === "string" ? frame.id : "";
      if (!id || !this.pending.has(id)) return;

      const pending = this.pending.get(id);
      this.pending.delete(id);
      if (pending.timer) clearTimeout(pending.timer);

      if (frame.ok) {
        pending.resolve(frame.payload);
        return;
      }

      const code = frame?.error?.code;
      const detailsCode = frame?.error?.details?.code;
      const message = frame?.error?.message || "gateway request failed";
      const error = new Error(`${pending.method} failed: ${message}`);
      error.code = code;
      error.detailsCode = detailsCode;

      if (detailsCode === "DEVICE_IDENTITY_REQUIRED" && (!this.config.enableDeviceAuth || !this.deviceIdentity)) {
        log("warn", "openclaw requires device identity", {
          hint: "set OPENCLAW_ENABLE_DEVICE_AUTH=true and ensure identity path is writable",
        });
      }
      if (detailsCode === "NOT_PAIRED" || code === "NOT_PAIRED") {
        log("warn", "openclaw reports device not paired", {
          hint: "approve pairing in OpenClaw before reconnecting",
        });
      }

      pending.reject(error);
    }
  }

  flushPending(error) {
    for (const [id, pending] of this.pending.entries()) {
      if (pending.timer) clearTimeout(pending.timer);
      pending.reject(error);
      this.pending.delete(id);
    }
  }

  scheduleReconnect() {
    if (this.closed) return;
    if (this.reconnectTimer) return;

    const jitterMs = Math.floor(Math.random() * 250);
    const delayMs = Math.min(this.backoffMs + jitterMs, this.config.reconnectMaxMs);
    log("info", "schedule openclaw reconnect", { delayMs });

    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, delayMs);
    this.reconnectTimer.unref?.();

    this.backoffMs = Math.min(this.backoffMs * 2, this.config.reconnectMaxMs);
  }
}

class HookHttpServer {
  constructor(config) {
    this.config = config;
    this.server = null;
    this.port = null;
  }

  start() {
    return new Promise((resolve, reject) => {
      const preferredPort = Number.isInteger(this.config.port) ? this.config.port : readRuntimePort() || DEFAULT_SERVER_PORT;
      const candidates = getPortCandidates(preferredPort);

      this.server = http.createServer((req, res) => this.handleRequest(req, res));

      const tryListen = (index) => {
        if (index >= candidates.length) {
          reject(new Error(`unable to bind http server to any candidate port: ${candidates.join(",")}`));
          return;
        }

        const candidate = candidates[index];
        const onError = (error) => {
          this.server.removeListener("listening", onListening);
          if (error && error.code === "EADDRINUSE") {
            tryListen(index + 1);
            return;
          }
          reject(error);
        };

        const onListening = () => {
          this.server.removeListener("error", onError);
          this.port = candidate;
          writeRuntimeConfig(candidate);
          resolve(candidate);
        };

        this.server.once("error", onError);
        this.server.once("listening", onListening);
        this.server.listen(candidate, "127.0.0.1");
      };

      tryListen(0);
    });
  }

  stop() {
    clearRuntimeConfig();
    if (!this.server) return;
    try { this.server.close(); } catch {}
    this.server = null;
  }

  sendJson(res, statusCode, body) {
    const payload = JSON.stringify(body);
    res.writeHead(statusCode, {
      "Content-Type": "application/json",
      [CLAWD_SERVER_HEADER]: CLAWD_SERVER_ID,
    });
    res.end(payload);
  }

  readRequestJson(req, maxBytes, callback) {
    let body = "";
    let bodySize = 0;
    let tooLarge = false;

    req.on("data", (chunk) => {
      if (tooLarge) return;
      bodySize += chunk.length;
      if (bodySize > maxBytes) {
        tooLarge = true;
        return;
      }
      body += chunk;
    });

    req.on("end", () => {
      if (tooLarge) {
        callback(new Error("payload too large"));
        return;
      }

      if (!body) {
        callback(null, {});
        return;
      }

      try {
        const data = JSON.parse(body);
        if (!data || typeof data !== "object") {
          callback(new Error("payload must be an object"));
          return;
        }
        callback(null, data);
      } catch (error) {
        callback(error);
      }
    });
  }

  handleRequest(req, res) {
    if (req.method === "GET" && req.url === "/state") {
      this.sendJson(res, 200, {
        ok: true,
        app: CLAWD_SERVER_ID,
        bridge: APP_NAME,
        version: APP_VERSION,
        port: this.port,
      });
      return;
    }

    if (req.method === "GET" && req.url === "/device-availability") {
      const devices = this.config.publisher
        ? this.config.publisher.getDeviceAvailability()
        : {};
      this.sendJson(res, 200, { ok: true, devices });
      return;
    }

    if (req.method === "POST" && req.url === "/publish-test") {
      this.readRequestJson(req, 16 * 1024, (error, payload) => {
        if (error) {
          this.sendJson(res, 400, { ok: false, error: String(error.message || error) });
          return;
        }
        const publisher = this.config.publisher;
        if (!publisher || !publisher.connected) {
          this.sendJson(res, 503, { ok: false, error: "MQTT not connected" });
          return;
        }
        const namespace = payload.namespace || "desk";
        const deviceId = payload.deviceId || "";
        const text = payload.text || "Pet Manager 配网测试 - 如果你看到这条消息，说明 MQTT 通信正常！";
        if (!deviceId) {
          this.sendJson(res, 400, { ok: false, error: "deviceId is required" });
          return;
        }
        const topic = `${namespace}/${deviceId}/speech/text`;
        const speechPayload = JSON.stringify({ text, source: "pet-manager-test", ts: nowIso() });
        publisher.client.publish(topic, speechPayload, { qos: 0, retain: false }, (err) => {
          if (err) {
            this.sendJson(res, 500, { ok: false, error: String(err) });
          } else {
            this.sendJson(res, 200, { ok: true, topic, text });
          }
        });
      });
      return;
    }

    if (req.method === "POST" && req.url === "/publish-command") {
      this.readRequestJson(req, 16 * 1024, (error, payload) => {
        if (error) {
          this.sendJson(res, 400, { ok: false, error: String(error.message || error) });
          return;
        }
        const publisher = this.config.publisher;
        if (!publisher || !publisher.connected) {
          this.sendJson(res, 503, { ok: false, error: "MQTT not connected" });
          return;
        }
        const boardDeviceId = payload.boardDeviceId || "";
        const command = payload.command || "";
        // Accept either the legacy {command} string form or a structured
        // {payload: {...}} pass-through (used by audio_bridge & friends so the
        // board can dispatch on `type` without us baking schemas in here).
        const rawPayload = payload.payload;
        if (!boardDeviceId || (!command && rawPayload == null)) {
          this.sendJson(res, 400, { ok: false, error: "boardDeviceId and command|payload are required" });
          return;
        }
        const topic = `claw-pet/board/${boardDeviceId}/control/command`;
        const cmdPayload = rawPayload != null
          ? JSON.stringify(typeof rawPayload === "string" ? { raw: rawPayload } : { ...rawPayload, ts: nowIso() })
          : JSON.stringify({ command, ts: nowIso() });
        publisher.client.publish(topic, cmdPayload, { qos: 1, retain: false }, (err) => {
          if (err) {
            this.sendJson(res, 500, { ok: false, error: String(err) });
          } else {
            this.sendJson(res, 200, { ok: true, topic, command: command || (rawPayload && rawPayload.type) || "" });
          }
        });
      });
      return;
    }

    if (req.method === "POST" && req.url === "/publish-remote-binding") {
      this.readRequestJson(req, 16 * 1024, (error, payload) => {
        if (error) {
          this.sendJson(res, 400, { ok: false, error: String(error.message || error) });
          return;
        }
        const publisher = this.config.publisher;
        if (!publisher || !publisher.connected) {
          this.sendJson(res, 503, { ok: false, error: "MQTT not connected" });
          return;
        }
        const namespace = normalizeTopicPart(payload.mqttNamespace || "desk", "desk");
        const boardDeviceId = normalizeTopicPart(payload.boardDeviceId || "", "");
        const binding = payload.binding && typeof payload.binding === "object" ? payload.binding : {};
        const targetDeviceId = normalizeTopicPart(binding.targetDeviceId || "", "");
        const targetSource = normalizeAgentId(binding.targetSource || "");
        const previousSource = normalizeAgentId(binding.previousSource || "");
        const targetBoards = publisher.resolveRemoteBindingTargets({
          boardDeviceId,
          targetDeviceId,
          mqttNamespace: namespace,
        });
        if (!targetDeviceId || !targetSource || targetBoards.length === 0) {
          this.sendJson(res, 400, {
            ok: false,
            error: "binding.targetDeviceId, binding.targetSource, and a matching online boardDeviceId are required",
          });
          return;
        }
        const bindingPayload = JSON.stringify({
          command: "remote_cli_binding.update",
          enabled: true,
          targetDeviceId,
          targetSource,
          mqttNamespace: namespace,
          updatedBy: binding.updatedBy || "pet-manager",
          ts: nowIso(),
          tsMs: Date.now(),
        });
        if (previousSource && previousSource !== targetSource) {
          publisher.publishJson(`${namespace}/${targetDeviceId}/state/${previousSource}`, {
            source: previousSource,
            state: "idle",
            reason: "source.disabled",
            event: "source.disabled",
            ts: nowIso(),
            tsMs: Date.now(),
          }, { retain: true });
        }
        const topics = targetBoards.map((target) => target.controlTopic);
        let pending = topics.length;
        const errors = [];
        const finish = () => {
          if (pending > 0) return;
          if (errors.length === topics.length) {
            this.sendJson(res, 500, { ok: false, error: errors.join("; ") });
          } else {
            this.sendJson(res, 200, {
              ok: true,
              topic: topics[0],
              topics,
              boardDeviceIds: targetBoards.map((target) => target.boardDeviceId),
              targetDeviceId,
              targetSource,
              mqttSent: true,
              usbSent: false,
            });
          }
        };
        topics.forEach((topic) => {
          publisher.client.publish(topic, bindingPayload, { qos: 1, retain: false }, (err) => {
            if (err) errors.push(String(err));
            pending -= 1;
            finish();
          });
        });
      });
      return;
    }

    if (req.method === "POST" && req.url === "/state") {
      this.readRequestJson(req, 64 * 1024, (error, payload) => {
        if (error) {
          this.sendJson(res, 400, { ok: false, error: String(error.message || error) });
          return;
        }

        this.config.onState(payload);
        this.sendJson(res, 200, { ok: true });
      });
      return;
    }

    if (req.method === "POST" && req.url === "/permission") {
      this.readRequestJson(req, 512 * 1024, (error, payload) => {
        if (error) {
          this.sendJson(res, 400, { ok: false, error: String(error.message || error) });
          return;
        }

        const decision = this.config.onPermission(payload);
        const responseBody = {
          hookSpecificOutput: {
            hookEventName: "PermissionRequest",
            decision,
          },
        };
        this.sendJson(res, 200, responseBody);
      });
      return;
    }

    if (req.method === "POST" && req.url === "/mock-button-inject") {
      this.readRequestJson(req, 64 * 1024, (error, payload) => {
        if (error) {
          this.sendJson(res, 400, { ok: false, error: String(error.message || error) });
          return;
        }
        if (typeof this.config.onMockButtonInject !== "function") {
          this.sendJson(res, 503, { ok: false, error: "mock button inject is unavailable" });
          return;
        }
        Promise.resolve(this.config.onMockButtonInject(payload))
          .then((result) => {
            this.sendJson(res, 200, {
              ok: true,
              action: "mock-button-inject",
              ...(result && typeof result === "object" ? result : {}),
            });
          })
          .catch((err) => {
            const statusCode = Number.isFinite(err?.statusCode) ? err.statusCode : 500;
            this.sendJson(res, statusCode, {
              ok: false,
              error: String(err?.message || err || "mock button inject failed"),
              code: typeof err?.code === "string" && err.code ? err.code : "MOCK_BUTTON_INJECT_FAILED",
            });
          });
      });
      return;
    }

    this.sendJson(res, 404, { ok: false, error: "not found" });
  }
}

function resolveMockButtonInjectRequest(payload, options = {}) {
  const safePayload = payload && typeof payload === "object" ? payload : {};
  const defaultText = typeof options.defaultText === "string" && options.defaultText.trim()
    ? options.defaultText.trim()
    : "这是设备按钮模拟输入，请继续当前会话并给出下一步。";
  const fallbackAgent = typeof options.defaultAgentId === "string" && options.defaultAgentId.trim()
    ? normalizeAgentId(options.defaultAgentId)
    : "codex";
  const defaultSessionId = typeof options.defaultSessionId === "string" && options.defaultSessionId.trim()
    ? options.defaultSessionId.trim()
    : "auto";

  const agentId = normalizeAgentId(
    typeof safePayload.agentId === "string" && safePayload.agentId.trim()
      ? safePayload.agentId.trim()
      : fallbackAgent
  );
  const sessionId = typeof safePayload.sessionId === "string" && safePayload.sessionId.trim()
    ? safePayload.sessionId.trim()
    : defaultSessionId;
  const text = typeof safePayload.text === "string" && safePayload.text.trim()
    ? safePayload.text.trim()
    : defaultText;
  const buttonEvent = typeof safePayload.buttonEvent === "string" && safePayload.buttonEvent.trim()
    ? safePayload.buttonEvent.trim()
    : "button.primary.short_press";
  const payloadMetadata = safePayload.metadata && typeof safePayload.metadata === "object" && !Array.isArray(safePayload.metadata)
    ? safePayload.metadata
    : {};

  const metadata = {
    ...payloadMetadata,
    source: typeof payloadMetadata.source === "string" && payloadMetadata.source.trim()
      ? payloadMetadata.source.trim()
      : "mock-button",
    inputType: "mock-text",
    trigger: "device-button",
    buttonEvent,
    ts: nowIso(),
  };

  return {
    agentId,
    sessionId,
    text,
    buttonEvent,
    metadata,
    injectBody: {
      agentId,
      sessionId,
      text,
      metadata,
    },
  };
}

function parseSseEventBlock(block) {
  const lines = String(block || "").split(/\r?\n/);
  let eventName = "";
  const dataLines = [];
  for (const line of lines) {
    if (!line || line.startsWith(":")) continue;
    if (line.startsWith("event:")) {
      eventName = line.slice(6).trim();
      continue;
    }
    if (line.startsWith("data:")) {
      dataLines.push(line.slice(5).trimStart());
    }
  }
  if (!eventName) return null;
  const rawData = dataLines.join("\n");
  let data = {};
  if (rawData) {
    try {
      data = JSON.parse(rawData);
    } catch {
      data = { raw: rawData };
    }
  }
  return { event: eventName, data };
}

function isSessionMetadataResumeError(error) {
  const text = [
    error?.message,
    error?.detail,
    error?.code,
  ].filter(Boolean).join("\n").toLowerCase();
  return text.includes("does not start with session metadata");
}

async function injectViaAgentBus(agentBus, injectBody, options = {}) {
  const body = injectBody && typeof injectBody === "object" ? injectBody : {};
  const requestedSessionId = typeof body.sessionId === "string" && body.sessionId.trim()
    ? body.sessionId.trim()
    : "auto";
  const canRetryFresh = options.retryFreshOnSessionMetadataError !== false
    && requestedSessionId === "auto";

  try {
    return await injectViaAgentBusOnce(agentBus, body, options);
  } catch (error) {
    if (!canRetryFresh || !isSessionMetadataResumeError(error)) {
      throw error;
    }
    const retryBody = {
      ...body,
      sessionId: "new",
      metadata: {
        ...(body.metadata && typeof body.metadata === "object" ? body.metadata : {}),
        recoveredFromSessionMetadataError: true,
      },
    };
    const retryResult = await injectViaAgentBusOnce(agentBus, retryBody, {
      ...options,
      retryFreshOnSessionMetadataError: false,
    });
    return {
      ...retryResult,
      recoveredSession: true,
      recoveredFromSessionId: requestedSessionId,
    };
  }
}

function injectViaAgentBusOnce(agentBus, injectBody, options = {}) {
  const timeoutMs = Number.isFinite(options.timeoutMs) && options.timeoutMs >= 2000
    ? Math.floor(options.timeoutMs)
    : 120000;
  const port = agentBus && typeof agentBus.port_ === "function"
    ? agentBus.port_()
    : null;
  if (!Number.isFinite(port) || port <= 0) {
    const error = new Error("agent-session-bus is not listening");
    error.code = "AGENT_BUS_UNAVAILABLE";
    error.statusCode = 503;
    throw error;
  }

  return new Promise((resolve, reject) => {
    let settled = false;
    const finish = (error, value) => {
      if (settled) return;
      settled = true;
      if (error) reject(error);
      else resolve(value);
    };

    const req = http.request({
      host: "127.0.0.1",
      port,
      method: "POST",
      path: "/agent/inject",
      headers: {
        "Content-Type": "application/json; charset=utf-8",
        Accept: "text/event-stream",
      },
    }, (res) => {
      const statusCode = Number.isFinite(res.statusCode) ? res.statusCode : 0;
      if (statusCode < 200 || statusCode >= 300) {
        const chunks = [];
        res.on("data", (chunk) => chunks.push(chunk));
        res.on("end", () => {
          const raw = Buffer.concat(chunks).toString("utf8");
          const error = new Error(`agent-session-bus inject failed (${statusCode})`);
          error.code = "AGENT_BUS_INJECT_HTTP_ERROR";
          error.statusCode = 502;
          error.detail = raw;
          finish(error);
        });
        return;
      }

      const summary = {
        ready: null,
        done: null,
        tokenPreview: "",
        tokenChars: 0,
      };
      let streamBuffer = "";

      const pushToken = (text) => {
        const chunk = typeof text === "string" ? text : "";
        summary.tokenChars += chunk.length;
        if (!chunk || summary.tokenPreview.length >= 512) return;
        const remaining = 512 - summary.tokenPreview.length;
        summary.tokenPreview += chunk.slice(0, remaining);
      };

      const consumeBlock = (block) => {
        const parsed = parseSseEventBlock(block);
        if (!parsed) return;
        const data = parsed.data && typeof parsed.data === "object" ? parsed.data : {};
        if (parsed.event === "ready") {
          summary.ready = data;
          return;
        }
        if (parsed.event === "token") {
          pushToken(data.text);
          return;
        }
        if (parsed.event === "error") {
          const error = new Error(
            typeof data.message === "string" && data.message
              ? data.message
              : "agent-session-bus returned error"
          );
          error.code = typeof data.code === "string" && data.code ? data.code : "AGENT_BUS_INJECT_ERROR";
          error.statusCode = 502;
          finish(error);
          return;
        }
        if (parsed.event === "done") {
          summary.done = data;
          finish(null, summary);
        }
      };

      res.on("data", (chunk) => {
        if (settled) return;
        streamBuffer += chunk.toString("utf8");
        let sep = streamBuffer.indexOf("\n\n");
        while (sep >= 0) {
          const block = streamBuffer.slice(0, sep);
          streamBuffer = streamBuffer.slice(sep + 2);
          consumeBlock(block);
          if (settled) return;
          sep = streamBuffer.indexOf("\n\n");
        }
      });

      res.on("end", () => {
        if (settled) return;
        if (summary.done) {
          finish(null, summary);
          return;
        }
        const error = new Error("agent-session-bus stream ended before done event");
        error.code = "AGENT_BUS_STREAM_ENDED";
        error.statusCode = 502;
        finish(error);
      });
    });

    req.on("timeout", () => {
      const error = new Error(`agent-session-bus inject timeout after ${timeoutMs}ms`);
      error.code = "AGENT_BUS_INJECT_TIMEOUT";
      error.statusCode = 504;
      req.destroy(error);
    });
    req.on("error", (error) => {
      const wrapped = error || new Error("agent-session-bus request failed");
      if (!wrapped.code) wrapped.code = "AGENT_BUS_REQUEST_FAILED";
      if (!Number.isFinite(wrapped.statusCode)) wrapped.statusCode = 502;
      finish(wrapped);
    });
    req.setTimeout(timeoutMs);
    req.write(JSON.stringify(injectBody));
    req.end();
  });
}

function resolvePermissionDecision(config, payload) {
  const behavior = config.permissionBehavior;
  if (behavior === "deny") {
    return {
      behavior: "deny",
      message: config.permissionDenyMessage,
    };
  }

  if (behavior === "allow") {
    return {
      behavior: "allow",
    };
  }

  log("warn", "invalid permission behavior, fallback to allow", {
    behavior,
    payloadSummary: {
      sessionId: payload?.session_id,
      toolName: payload?.tool_name,
    },
  });
  return { behavior: "allow" };
}

function publishClawdState(config, payload) {
  const clawdState = typeof payload.state === "string" ? payload.state : "";
  const event = typeof payload.event === "string" ? payload.event : "";
  const source = normalizeAgentId(payload.agent_id || "claude-code");
  const status = normalizeStatus(clawdState) || mapClawdStateToStatus(clawdState, event) || "idle";
  const tokenUsage = extractTokenUsageFromPayload(payload);

  const out = {
    source,
    channel: "clawd-hook",
    bridge: APP_NAME,
    bridgeVersion: APP_VERSION,
    state: status,
    rawState: clawdState || undefined,
    reason: `clawd.${event || clawdState || "unknown"}`,
    event: event || undefined,
    sessionId: typeof payload.session_id === "string" ? payload.session_id : undefined,
    cwd: typeof payload.cwd === "string" ? payload.cwd : undefined,
    host: typeof payload.host === "string" ? payload.host : undefined,
    headless: payload.headless === true,
    sourcePid: Number.isFinite(payload.source_pid) ? payload.source_pid : undefined,
    agentPid: Number.isFinite(payload.agent_pid) ? payload.agent_pid : undefined,
    tokenUsage: tokenUsage || undefined,
    ts: nowIso(),
    tsMs: Date.now(),
    ...(config.includeRaw ? { raw: payload } : {}),
  };

  const enriched = enrichPayloadMetrics(config.metricsTracker, out);
  config.publisher.publishSource(enriched);
}

function publishPermissionRequest(config, payload) {
  const out = {
    source: "claude-code",
    channel: "clawd-permission",
    bridge: APP_NAME,
    bridgeVersion: APP_VERSION,
    state: "waiting_user",
    rawState: "permission_request",
    reason: "clawd.PermissionRequest",
    event: "PermissionRequest",
    sessionId: typeof payload.session_id === "string" ? payload.session_id : undefined,
    detail: {
      toolName: typeof payload.tool_name === "string" ? payload.tool_name : undefined,
      hasToolInput: payload.tool_input !== undefined,
      hasSuggestions: Array.isArray(payload.permission_suggestions) && payload.permission_suggestions.length > 0,
    },
    ts: nowIso(),
    tsMs: Date.now(),
    ...(config.includeRaw ? { raw: payload } : {}),
  };

  const enriched = enrichPayloadMetrics(config.metricsTracker, out);
  config.publisher.publishSource(enriched);
}

function syncHooks(port, autoStart) {
  const outcomes = [];

  try {
    const { registerHooks } = require("../../../hooks/install.js");
    const result = registerHooks({
      silent: true,
      autoStart,
      port,
    });
    outcomes.push({ name: "claude", result });
  } catch (error) {
    outcomes.push({ name: "claude", error: String(error.message || error) });
  }

  try {
    const { registerGeminiHooks } = require("../../../hooks/gemini-install.js");
    const result = registerGeminiHooks({ silent: true });
    outcomes.push({ name: "gemini", result });
  } catch (error) {
    outcomes.push({ name: "gemini", error: String(error.message || error) });
  }

  try {
    const { registerCursorHooks } = require("../../../hooks/cursor-install.js");
    const result = registerCursorHooks({ silent: true });
    outcomes.push({ name: "cursor", result });
  } catch (error) {
    outcomes.push({ name: "cursor", error: String(error.message || error) });
  }

  try {
    const { registerCodeBuddyHooks } = require("../../../hooks/codebuddy-install.js");
    const result = registerCodeBuddyHooks({ silent: true });
    outcomes.push({ name: "codebuddy", result });
  } catch (error) {
    outcomes.push({ name: "codebuddy", error: String(error.message || error) });
  }

  for (const item of outcomes) {
    if (item.error) {
      log("warn", "hook sync failed", { source: item.name, error: item.error });
      continue;
    }
    log("info", "hook sync complete", {
      source: item.name,
      added: item.result?.added,
      updated: item.result?.updated,
      removed: item.result?.removed,
      skipped: item.result?.skipped,
    });
  }
}

function startCodexMonitor(config) {
  let monitor = null;
  const lastSpeechSignatureBySession = new Map();
  const visibleCodexEvents = new Set([
    "event_msg:task_started",
    "event_msg:agent_message",
    "event_msg:task_complete",
  ]);
  const speechCodexEvents = new Set([
    "event_msg:agent_message",
    "event_msg:task_complete",
  ]);
  const allowedCodexEvents = new Set([
    ...visibleCodexEvents,
    "event_msg:token_count",
  ]);
  try {
    const CodexLogMonitor = require("../../../agents/codex-log-monitor");
    const codexAgent = require("../../../agents/codex");

    monitor = new CodexLogMonitor(codexAgent, (sessionId, state, event, extra) => {
      if (!allowedCodexEvents.has(event)) return;
      const isVisibleEvent = visibleCodexEvents.has(event);
      const normalized = state === "codex-permission"
        ? "waiting_user"
        : mapClawdStateToStatus(state, event) || "idle";

      const out = {
        source: "codex",
        channel: "codex-log",
        bridge: APP_NAME,
        bridgeVersion: APP_VERSION,
        state: normalized,
        rawState: state,
        reason: state === "codex-permission" ? "codex.permission" : `codex.${event || state}`,
        event: event || undefined,
        sessionId,
        sessionTitle: extra && typeof extra.sessionTitle === "string" ? extra.sessionTitle : undefined,
        cwd: extra && typeof extra.cwd === "string" ? extra.cwd : undefined,
        display: isVisibleEvent && extra && extra.display && typeof extra.display === "object" ? extra.display : undefined,
        session: extra && extra.session && typeof extra.session === "object" ? extra.session : undefined,
        turn: extra && extra.turn && typeof extra.turn === "object" ? extra.turn : undefined,
        messages: isVisibleEvent && extra && extra.messages && typeof extra.messages === "object" ? extra.messages : undefined,
        tokenUsage: extra && extra.tokenUsage && typeof extra.tokenUsage === "object"
          ? extra.tokenUsage
          : undefined,
        ts: nowIso(),
        tsMs: Date.now(),
      };

      const enriched = enrichPayloadMetrics(config.metricsTracker, out);
      config.publisher.publishSource(enriched);
      if (speechCodexEvents.has(event) && enriched.display && (enriched.display.title || enriched.display.content)) {
        const speechSignature = JSON.stringify({
          sessionId: sessionId || "",
          title: enriched.display.title || "",
          content: enriched.display.content || "",
          status: normalized,
        });
        const speechKey = sessionId || "codex:session:auto";
        if (speechSignature !== lastSpeechSignatureBySession.get(speechKey)) {
          lastSpeechSignatureBySession.set(speechKey, speechSignature);
          config.publisher.publishSpeech("codex", {
            displayTitle: enriched.display.title || "",
            displayContent: enriched.display.content || "",
            status: normalized,
            event: event || undefined,
            sessionId,
            sessionTitle: enriched.sessionTitle,
            ts: enriched.ts,
            tsMs: enriched.tsMs,
            expiresAtMs: enriched.tsMs + SPEECH_EXPIRES_MS,
          });
        }
      }
    });

    monitor.start();
    log("info", "codex log monitor started", {});
  } catch (error) {
    log("warn", "codex log monitor not started", { error: String(error.message || error) });
  }

  return {
    stop() {
      if (!monitor) return;
      try { monitor.stop(); } catch {}
      monitor = null;
    },
  };
}

function startClaudeLogMonitor(config) {
  let monitor = null;
  const lastSpeechSignatureBySession = new Map();
  const speechClaudeEvents = new Set([
    "claude:assistant_message",
  ]);
  try {
    const ClaudeLogMonitor = require("../../../agents/claude-log-monitor");
    const claudeAgent = require("../../../agents/claude-code");

    monitor = new ClaudeLogMonitor(claudeAgent, (sessionId, state, event, extra) => {
      const normalized = mapClawdStateToStatus(state, event) || "idle";
      const out = {
        source: "claude-code",
        channel: "claude-log",
        bridge: APP_NAME,
        bridgeVersion: APP_VERSION,
        state: normalized,
        rawState: state,
        reason: `claude.log.${event || state}`,
        event: event || undefined,
        sessionId,
        sessionTitle: extra && typeof extra.sessionTitle === "string" ? extra.sessionTitle : undefined,
        cwd: extra && typeof extra.cwd === "string" ? extra.cwd : undefined,
        display: extra && extra.display && typeof extra.display === "object" ? extra.display : undefined,
        session: extra && extra.session && typeof extra.session === "object" ? extra.session : undefined,
        messages: extra && extra.messages && typeof extra.messages === "object" ? extra.messages : undefined,
        tokenUsage: extra && extra.tokenUsage && typeof extra.tokenUsage === "object"
          ? extra.tokenUsage
          : undefined,
        ts: nowIso(),
        tsMs: Date.now(),
        ...(config.includeRaw ? { raw: extra } : {}),
      };

      const enriched = enrichPayloadMetrics(config.metricsTracker, out);
      config.publisher.publishSource(enriched);
      if (speechClaudeEvents.has(event) && enriched.display && (enriched.display.title || enriched.display.content)) {
        const speechSignature = JSON.stringify({
          sessionId: sessionId || "",
          title: enriched.display.title || "",
          content: enriched.display.content || "",
          status: normalized,
        });
        const speechKey = sessionId || "claude:session:auto";
        if (speechSignature !== lastSpeechSignatureBySession.get(speechKey)) {
          lastSpeechSignatureBySession.set(speechKey, speechSignature);
          config.publisher.publishSpeech("claude-code", {
            displayTitle: enriched.display.title || "",
            displayContent: enriched.display.content || "",
            status: normalized,
            event: event || undefined,
            sessionId,
            sessionTitle: enriched.sessionTitle,
            ts: enriched.ts,
            tsMs: enriched.tsMs,
            expiresAtMs: enriched.tsMs + SPEECH_EXPIRES_MS,
          });
        }
      }
    });

    monitor.start();
    log("info", "claude log monitor started", {});
  } catch (error) {
    log("warn", "claude log monitor not started", { error: String(error.message || error) });
  }

  return {
    stop() {
      if (!monitor) return;
      try { monitor.stop(); } catch {}
      monitor = null;
    },
  };
}

function resolveConfig() {
  const defaultDeviceId = normalizeTopicPart(os.hostname().toLowerCase(), "devbox");
  const stateDirFallback = path.join(os.homedir(), ".status-bridge");

  return {
    includeRaw: getEnvBool("STATUS_INCLUDE_RAW", false),
    mqtt: {
      url: getEnv("MQTT_URL", "mqtt://127.0.0.1:1883"),
      username: getEnv("MQTT_USERNAME", ""),
      password: getEnv("MQTT_PASSWORD", ""),
      clientId: normalizeTopicPart(
        getEnv("MQTT_CLIENT_ID", `${APP_NAME}-${defaultDeviceId}-${process.pid}`),
        `${APP_NAME}-${process.pid}`,
      ),
      reconnectMs: Math.max(500, getEnvInt("MQTT_RECONNECT_MS", 1000)),
      qos: Math.min(2, Math.max(0, getEnvInt("MQTT_QOS", 1))),
      retain: getEnvBool("MQTT_RETAIN", true),
      namespace: getEnv("STATUS_NAMESPACE", "desk"),
      deviceId: normalizeTopicPart(getEnv("STATUS_DEVICE_ID", defaultDeviceId), defaultDeviceId),
      enableActiveTopic: getEnvBool("STATUS_ENABLE_ACTIVE_TOPIC", false),
      activeTopicSuffix: getEnv("STATUS_ACTIVE_TOPIC_SUFFIX", "active"),
      enabledSources: parseCsvEnv("CLAWD_ENABLED_AGENTS", []),
      selectedSource: getEnv("CLAWD_SELECTED_AGENT_ID", ""),
      localStateDir: getEnv(
        "STATUS_BRIDGE_LOCAL_STATE_DIR",
        path.join(os.tmpdir(), "pet-manager-bridge-state"),
      ),
    },
    http: {
      port: getEnvInt("CLAWD_BRIDGE_PORT", readRuntimePort() || DEFAULT_SERVER_PORT),
      syncHooks: getEnvBool("CLAWD_SYNC_HOOKS", true),
      autoStartHook: getEnvBool("CLAWD_AUTO_START_HOOK", false),
      permissionBehavior: getEnv("CLAWD_PERMISSION_BEHAVIOR", "allow").toLowerCase(),
      permissionDenyMessage: getEnv("CLAWD_PERMISSION_DENY_MESSAGE", "Denied by bridge policy"),
      mockButtonDefaultText: getEnv(
        "CLAWD_MOCK_BUTTON_TEXT",
        "这是设备按钮模拟输入，请继续当前会话并给出下一步。",
      ),
      mockButtonDefaultAgentId: getEnv("CLAWD_MOCK_BUTTON_AGENT_ID", ""),
      mockButtonDefaultSessionId: getEnv("CLAWD_MOCK_BUTTON_SESSION_ID", "auto"),
      mockButtonTimeoutMs: Math.max(2000, getEnvInt("CLAWD_MOCK_BUTTON_TIMEOUT_MS", 120000)),
    },
    codex: {
      enabled: getEnvBool("CLAWD_ENABLE_CODEX_MONITOR", true),
    },
    claude: {
      enabledLogMonitor: getEnvBool("CLAWD_ENABLE_CLAUDE_LOG_MONITOR", true),
    },
    openclaw: {
      enabled: getEnvBool("OPENCLAW_ENABLE", true),
      gatewayUrl: getEnv("OPENCLAW_GATEWAY_URL", "ws://127.0.0.1:18789"),
      token: getEnv("OPENCLAW_GATEWAY_TOKEN", ""),
      password: getEnv("OPENCLAW_GATEWAY_PASSWORD", ""),
      deviceToken: getEnv("OPENCLAW_GATEWAY_DEVICE_TOKEN", ""),
      protocolVersion: Math.max(1, getEnvInt("OPENCLAW_PROTOCOL_VERSION", PROTOCOL_VERSION_FALLBACK)),
      challengeTimeoutMs: Math.max(500, getEnvInt("OPENCLAW_CHALLENGE_TIMEOUT_MS", 5000)),
      connectTimeoutMs: Math.max(1000, getEnvInt("OPENCLAW_CONNECT_TIMEOUT_MS", 10000)),
      requestTimeoutMs: Math.max(1000, getEnvInt("OPENCLAW_REQUEST_TIMEOUT_MS", 10000)),
      reconnectMinMs: Math.max(500, getEnvInt("OPENCLAW_RECONNECT_MIN_MS", 1000)),
      reconnectMaxMs: Math.max(1000, getEnvInt("OPENCLAW_RECONNECT_MAX_MS", 30000)),
      subscribeSessions: getEnvBool("OPENCLAW_STATUS_SESSION_SUBSCRIBE", true),
      clientId: getEnv("OPENCLAW_CLIENT_ID", "status-bridge"),
      clientMode: getEnv("OPENCLAW_CLIENT_MODE", "backend"),
      platform: getEnv("OPENCLAW_CLIENT_PLATFORM", process.platform),
      deviceFamily: getEnv("OPENCLAW_DEVICE_FAMILY", "status-bridge"),
      instanceId: getEnv("OPENCLAW_INSTANCE_ID", crypto.randomUUID()),
      locale: getEnv("OPENCLAW_LOCALE", "zh-CN"),
      scopes: parseCsvEnv("OPENCLAW_SCOPES", ["operator.read"]),
      enableDeviceAuth: getEnvBool("OPENCLAW_ENABLE_DEVICE_AUTH", true),
      deviceIdentityPath: getEnv(
        "OPENCLAW_DEVICE_IDENTITY_PATH",
        path.join(getEnv("STATUS_BRIDGE_STATE_DIR", stateDirFallback), "openclaw-device.json"),
      ),
    },
    openclawState: {
      idleTimeoutMs: Math.max(1000, getEnvInt("STATUS_IDLE_TIMEOUT_MS", 15000)),
      idleWatchTickMs: Math.max(500, getEnvInt("STATUS_IDLE_WATCH_TICK_MS", 1000)),
      heartbeatMs: Math.max(0, getEnvInt("STATUS_HEARTBEAT_MS", 30000)),
    },
  };
}

function ensureDependencies(config) {
  if (!mqtt) {
    throw new Error('missing dependency "mqtt". run: npm install mqtt ws');
  }
  if (config.openclaw.enabled && !WebSocket) {
    throw new Error('missing dependency "ws" while OPENCLAW_ENABLE=true. run: npm install ws');
  }
}

// Lazy-require the agent-session-bus so a missing/broken sibling package
// doesn't crash the whole bridge — voice will simply be unavailable.
function tryLoadAgentSessionBus() {
  try {
    return require("../../agent-session-bus/src/index.js");
  } catch (error) {
    log("warn", "agent-session-bus failed to load; voice disabled", {
      error: String(error && error.message ? error.message : error),
    });
    return null;
  }
}

async function main() {
  const config = resolveConfig();
  ensureDependencies(config);

  const metricsTracker = new SessionMetricsTracker();
  let agentBus = null;
  let publisher = null;

  const onBoardInputAction = async ({ boardDeviceId, payload, topic }) => {
    const data = payload && typeof payload === "object" ? payload : {};
    const view = typeof data.view === "string" ? data.view.trim().toLowerCase() : "";
    const text = typeof data.state === "string" ? data.state.trim() : "";
    if (view !== "voice_input" || !text) return;

    if (!agentBus) {
      log("warn", "board voice input dropped: agent-session-bus unavailable", { boardDeviceId, topic });
      return;
    }

    const desktopDeviceId = normalizeTopicPart(config.mqtt.deviceId, "");
    const boardStatus = publisher ? publisher.getBoardAvailabilityStatus(boardDeviceId) : null;
    const boundDesktopId = normalizeTopicPart(
      (boardStatus && (boardStatus.targetDeviceId || boardStatus.desktopDeviceId)) || "",
      "",
    );
    if (boundDesktopId && desktopDeviceId && boundDesktopId !== desktopDeviceId) {
      log("info", "ignore board voice input for another desktop", {
        boardDeviceId,
        boundDesktopId,
        desktopDeviceId,
      });
      return;
    }

    const agentId = normalizeAgentId(
      (boardStatus && boardStatus.targetSource)
      || (publisher && publisher.getPreferredSource())
      || config.http.mockButtonDefaultAgentId
      || "codex"
    );
    const injectBody = {
      agentId,
      sessionId: "auto",
      text,
      metadata: {
        source: "board-voice-ptt",
        inputType: "voice-text",
        trigger: "device-button",
        boardDeviceId: normalizeTopicPart(data.boardDeviceId || boardDeviceId || "", ""),
        localDeviceId: normalizeTopicPart(data.localDeviceId || "", ""),
        actionType: typeof data.type === "string" ? data.type : "",
        ts: nowIso(),
      },
    };
    const injected = await injectViaAgentBus(agentBus, injectBody, {
      timeoutMs: config.http.mockButtonTimeoutMs,
    });
    log("info", "board voice input injected", {
      boardDeviceId,
      agentId,
      sessionId: injected?.done?.sessionId || injected?.ready?.sessionId || "auto",
      runId: injected?.ready?.runId || "",
      chars: text.length,
    });
    if (publisher && injected?.tokenPreview) {
      publisher.publishSpeech(agentId, {
        displayTitle: "Voice Reply",
        displayContent: injected.tokenPreview,
        source: "board-voice-ptt",
        boardDeviceId,
        ts: nowIso(),
        tsMs: Date.now(),
      });
    }
  };

  publisher = new MqttPublisher({
    ...config.mqtt,
    onBoardInputAction,
  });
  publisher.start();

  const server = new HookHttpServer({
    port: config.http.port,
    publisher,
    onState: config.http.syncHooks
      ? (payload) => publishClawdState({
          publisher,
          metricsTracker,
          includeRaw: config.includeRaw,
        }, payload)
      : () => {},
    onPermission: config.http.syncHooks
      ? (payload) => {
          publishPermissionRequest({
            publisher,
            metricsTracker,
            includeRaw: config.includeRaw,
          }, payload);
          return resolvePermissionDecision(config.http, payload);
        }
      : () => ({ allow: false, reason: "hook sync disabled" }),
    onMockButtonInject: async (payload) => {
      if (!agentBus) {
        const error = new Error("agent-session-bus unavailable");
        error.code = "AGENT_BUS_UNAVAILABLE";
        error.statusCode = 503;
        throw error;
      }
      const resolved = resolveMockButtonInjectRequest(payload, {
        defaultAgentId:
          (typeof config.http.mockButtonDefaultAgentId === "string" && config.http.mockButtonDefaultAgentId.trim())
            ? config.http.mockButtonDefaultAgentId.trim()
            : (publisher.getPreferredSource() || "codex"),
        defaultSessionId: config.http.mockButtonDefaultSessionId,
        defaultText: config.http.mockButtonDefaultText,
      });
      const injected = await injectViaAgentBus(agentBus, resolved.injectBody, {
        timeoutMs: config.http.mockButtonTimeoutMs,
      });
      log("info", "mock button inject completed", {
        agentId: resolved.agentId,
        sessionId: injected?.done?.sessionId || injected?.ready?.sessionId || resolved.sessionId,
        runId: injected?.ready?.runId,
      });
      return {
        request: {
          agentId: resolved.agentId,
          sessionId: resolved.sessionId,
          buttonEvent: resolved.buttonEvent,
          text: resolved.text,
        },
        ready: injected.ready,
        done: injected.done,
        tokenPreview: injected.tokenPreview,
        tokenChars: injected.tokenChars,
      };
    },
  });

  const port = await server.start();
  log("info", "http bridge server started", {
    port,
    statePath: "http://127.0.0.1:" + port + "/state",
    permissionPath: "http://127.0.0.1:" + port + "/permission",
    mockButtonInjectPath: "http://127.0.0.1:" + port + "/mock-button-inject",
  });

  if (config.http.syncHooks) {
    syncHooks(port, config.http.autoStartHook);
  } else {
    log("info", "hook sync skipped", { reason: "CLAWD_SYNC_HOOKS=false" });
  }

  const codexMonitor = config.codex.enabled
    ? startCodexMonitor({ publisher, metricsTracker, includeRaw: config.includeRaw })
    : { stop() {} };
  const claudeLogMonitor = config.claude.enabledLogMonitor
    ? startClaudeLogMonitor({ publisher, metricsTracker, includeRaw: config.includeRaw })
    : { stop() {} };

  let openclawStatus = null;
  let openclawBridge = null;

  if (config.openclaw.enabled) {
    openclawStatus = new OpenClawStatusController({
      ...config.openclawState,
      includeRaw: config.includeRaw,
      publisher,
      metricsTracker,
    });
    openclawStatus.start();

    openclawBridge = new OpenClawGatewayBridge({
      ...config.openclaw,
      onEvent: (event, payload) => openclawStatus.consumeEvent(event, payload),
      onConnectionState: (connected) => openclawStatus.setGatewayConnected(connected),
    });
    openclawBridge.start();
  } else {
    log("info", "openclaw bridge disabled", { reason: "OPENCLAW_ENABLE=false" });
  }

  // ── Agent Session Bus ────────────────────────────────────────────────
  // Voice service (voice-service-node) connects to the bus over HTTP+SSE
  // at ${VOICE_BUS_URL}/agent/inject and the bus dispatches turns into
  // the user's currently selected coding agent (Claude / Codex / OpenClaw).
  // See docs/voice-architecture.md for the full design.
  if (process.env.AGENT_BUS_DISABLED !== "1") {
    const busModule = tryLoadAgentSessionBus();
    if (busModule) {
      const {
        createAgentSessionBus,
        ClaudeCodeAdapter,
        CodexAdapter,
        OpenClawAdapter,
      } = busModule;
      const busLog = (level, message, details) => log(level, `bus :: ${message}`, details);
      const adapters = [
        new ClaudeCodeAdapter({ log: busLog }),
        new CodexAdapter({ log: busLog }),
        new OpenClawAdapter({ log: busLog }),
      ];
      agentBus = createAgentSessionBus({ adapters, log: busLog });
      try {
        const port = await agentBus.start();
        log("info", "agent-session-bus listening", { port, adapters: adapters.map((a) => a.agentId) });
      } catch (error) {
        log("error", "agent-session-bus failed to start", {
          error: String(error && error.message ? error.message : error),
        });
        agentBus = null;
      }
    }
  } else {
    log("info", "agent-session-bus disabled", { reason: "AGENT_BUS_DISABLED=1" });
  }

  function shutdown(signal) {
    log("info", "shutdown requested", { signal });
    if (agentBus) {
      Promise.resolve(agentBus.stop()).catch(() => {});
    }
    if (openclawBridge) openclawBridge.stop();
    if (openclawStatus) openclawStatus.stop();
    codexMonitor.stop();
    claudeLogMonitor.stop();
    server.stop();
    publisher.stop();
    setTimeout(() => process.exit(0), 50).unref?.();
  }

  process.on("SIGINT", () => shutdown("SIGINT"));
  process.on("SIGTERM", () => shutdown("SIGTERM"));

  log("info", "bridge boot complete", {
    app: APP_NAME,
    version: APP_VERSION,
    mqttUrl: config.mqtt.url,
    baseTopic: `${normalizeTopicPart(config.mqtt.namespace, "desk")}/${normalizeTopicPart(config.mqtt.deviceId, "devbox")}`,
    codexMonitor: config.codex.enabled,
    claudeLogMonitor: config.claude.enabledLogMonitor,
    openclawEnabled: config.openclaw.enabled,
    agentBusEnabled: Boolean(agentBus),
  });
}

if (require.main === module) {
  main().catch((error) => {
    log("error", "bridge failed", {
      error: String(error && error.stack ? error.stack : error),
    });
    process.exit(1);
  });
}

module.exports = {
  MqttPublisher,
  HookHttpServer,
  mapClawdStateToStatus,
  normalizeStatus,
  resolveMockButtonInjectRequest,
  injectViaAgentBus,
};
