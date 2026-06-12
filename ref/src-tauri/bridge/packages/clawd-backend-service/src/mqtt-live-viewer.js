#!/usr/bin/env node
"use strict";

const fs = require("fs");
const http = require("http");
const path = require("path");
const process = require("process");
const os = require("os");

const mqtt = require("mqtt");

function readBooleanEnv(name, fallback) {
  const normalized = String(process.env[name] || "").trim().toLowerCase();
  if (!normalized) return fallback;
  if (["1", "true", "yes", "on"].includes(normalized)) return true;
  if (["0", "false", "no", "off"].includes(normalized)) return false;
  return fallback;
}

function readIntegerEnv(name, fallback) {
  const parsed = Number.parseInt(process.env[name] || "", 10);
  return Number.isInteger(parsed) && parsed > 0 ? parsed : fallback;
}

const MQTT_URL = process.env.MQTT_URL || "mqtt://127.0.0.1:1883";
const MQTT_USERNAME = process.env.MQTT_USERNAME || "";
const MQTT_PASSWORD = process.env.MQTT_PASSWORD || "";
const MQTT_TOPIC = process.env.MQTT_TOPIC || "desk/+/+/#";
const MQTT_CLIENT_ID = process.env.MQTT_CLIENT_ID || `mqtt-live-viewer-${os.hostname()}-${process.pid}`;
const MQTT_RECONNECT_MS = Math.max(500, readIntegerEnv("MQTT_RECONNECT_MS", 1500));
const WEB_PORT = Number.parseInt(process.env.VIEWER_HTTP_PORT || "23999", 10);
const MAX_BUFFER = Math.max(50, Number.parseInt(process.env.VIEWER_MAX_BUFFER || "500", 10));
const MQTT_CONNECT_TIMEOUT_MS = Math.max(1000, readIntegerEnv("MQTT_CONNECT_TIMEOUT_MS", 10000));
const MQTT_KEEPALIVE_SECONDS = Math.max(5, readIntegerEnv("MQTT_KEEPALIVE_SECONDS", 30));
const MQTT_PROTOCOL_VERSION = Math.max(3, Math.min(5, readIntegerEnv("MQTT_PROTOCOL_VERSION", 5)));
const MQTT_CLEAN = readBooleanEnv("MQTT_CLEAN", true);
const MQTT_RESUBSCRIBE = readBooleanEnv("MQTT_RESUBSCRIBE", true);
const MQTT_LOG_PACKETS = readBooleanEnv("MQTT_LOG_PACKETS", false);

const htmlPath = path.join(__dirname, "mqtt-live-viewer.html");

const clients = new Set();
const history = [];

const state = {
  mqttConnected: false,
  mqttUrl: MQTT_URL,
  mqttTopic: MQTT_TOPIC,
  clientId: MQTT_CLIENT_ID,
  reconnectPeriodMs: MQTT_RECONNECT_MS,
  connectTimeoutMs: MQTT_CONNECT_TIMEOUT_MS,
  keepaliveSeconds: MQTT_KEEPALIVE_SECONDS,
  protocolVersion: MQTT_PROTOCOL_VERSION,
  clean: MQTT_CLEAN,
  resubscribe: MQTT_RESUBSCRIBE,
  messages: 0,
  startedAt: new Date().toISOString(),
  lastMessageAt: null,
  lastError: null,
  reconnectCount: 0,
  lastConnectAt: null,
  lastConnackAt: null,
  lastDisconnectAt: null,
  lastOfflineAt: null,
  lastErrorAt: null,
  lastDisconnectReason: null,
};

function pushHistory(item) {
  history.push(item);
  if (history.length > MAX_BUFFER) {
    history.shift();
  }
}

function sseWrite(client, event, payload) {
  client.write(`event: ${event}\n`);
  client.write(`data: ${JSON.stringify(payload)}\n\n`);
}

function broadcast(event, payload) {
  for (const client of clients) {
    try {
      sseWrite(client, event, payload);
    } catch {}
  }
}

function sendSnapshot(client) {
  sseWrite(client, "snapshot", {
    state,
    history,
  });
}

const mqttClient = mqtt.connect(MQTT_URL, {
  clientId: MQTT_CLIENT_ID,
  clean: MQTT_CLEAN,
  resubscribe: MQTT_RESUBSCRIBE,
  username: MQTT_USERNAME || undefined,
  password: MQTT_PASSWORD || undefined,
  reconnectPeriod: MQTT_RECONNECT_MS,
  connectTimeout: MQTT_CONNECT_TIMEOUT_MS,
  keepalive: MQTT_KEEPALIVE_SECONDS,
  protocolVersion: MQTT_PROTOCOL_VERSION,
});

mqttClient.on("connect", (connack) => {
  state.mqttConnected = true;
  state.lastError = null;
  state.lastConnectAt = new Date().toISOString();
  state.lastConnackAt = state.lastConnectAt;
  state.lastDisconnectReason = null;
  mqttClient.subscribe(MQTT_TOPIC, { qos: 1 }, (error) => {
    if (error) {
      state.lastError = `subscribe failed: ${error.message || String(error)}`;
      state.lastErrorAt = new Date().toISOString();
      broadcast("status", state);
      return;
    }
    console.log(JSON.stringify({
      ts: new Date().toISOString(),
      level: "info",
      message: "mqtt viewer connected",
      mqttUrl: MQTT_URL,
      topic: MQTT_TOPIC,
      clientId: MQTT_CLIENT_ID,
      sessionPresent: Boolean(connack && connack.sessionPresent),
      returnCode: connack && Number.isInteger(connack.returnCode) ? connack.returnCode : undefined,
      reasonCode: connack && Number.isInteger(connack.reasonCode) ? connack.reasonCode : undefined,
    }));
    broadcast("status", state);
  });
});

mqttClient.on("reconnect", () => {
  state.mqttConnected = false;
  state.reconnectCount += 1;
  broadcast("status", state);
});

mqttClient.on("close", () => {
  state.mqttConnected = false;
  state.lastDisconnectAt = new Date().toISOString();
  broadcast("status", state);
});

mqttClient.on("offline", () => {
  state.mqttConnected = false;
  state.lastOfflineAt = new Date().toISOString();
  broadcast("status", state);
});

mqttClient.on("disconnect", (packet) => {
  state.mqttConnected = false;
  state.lastDisconnectAt = new Date().toISOString();
  state.lastDisconnectReason = packet && Number.isInteger(packet.reasonCode) ? `reasonCode=${packet.reasonCode}` : "server disconnect";
  broadcast("status", state);
});

mqttClient.on("error", (error) => {
  state.lastError = `${error.message || String(error)} (clientId=${MQTT_CLIENT_ID}, timeout=${MQTT_CONNECT_TIMEOUT_MS}ms, protocol=${MQTT_PROTOCOL_VERSION})`;
  state.lastErrorAt = new Date().toISOString();
  console.error(JSON.stringify({
    ts: state.lastErrorAt,
    level: "error",
    message: "mqtt viewer error",
    mqttUrl: MQTT_URL,
    topic: MQTT_TOPIC,
    clientId: MQTT_CLIENT_ID,
    connectTimeoutMs: MQTT_CONNECT_TIMEOUT_MS,
    keepaliveSeconds: MQTT_KEEPALIVE_SECONDS,
    protocolVersion: MQTT_PROTOCOL_VERSION,
    clean: MQTT_CLEAN,
    resubscribe: MQTT_RESUBSCRIBE,
    error: error && error.message ? error.message : String(error),
  }));
  broadcast("status", state);
});

if (MQTT_LOG_PACKETS) {
  mqttClient.on("packetsend", (packet) => {
    const cmd = String((packet && packet.cmd) || "").toLowerCase();
    if (["connect", "disconnect", "pingreq", "subscribe"].includes(cmd)) {
      console.log(JSON.stringify({
        ts: new Date().toISOString(),
        level: "info",
        message: "mqtt viewer packetsend",
        cmd,
        messageId: packet && packet.messageId ? packet.messageId : undefined,
      }));
    }
  });

  mqttClient.on("packetreceive", (packet) => {
    const cmd = String((packet && packet.cmd) || "").toLowerCase();
    if (["connack", "disconnect", "pingresp", "suback"].includes(cmd)) {
      console.log(JSON.stringify({
        ts: new Date().toISOString(),
        level: "info",
        message: "mqtt viewer packetreceive",
        cmd,
        reasonCode: packet && Number.isInteger(packet.reasonCode) ? packet.reasonCode : undefined,
        returnCode: packet && Number.isInteger(packet.returnCode) ? packet.returnCode : undefined,
        messageId: packet && packet.messageId ? packet.messageId : undefined,
      }));
    }
  });
}

mqttClient.on("message", (topic, payloadBuffer) => {
  let payloadText = payloadBuffer.toString("utf8");
  let json = null;
  try {
    json = JSON.parse(payloadText);
  } catch {}

  const item = {
    ts: new Date().toISOString(),
    topic,
    payloadText,
    payloadJson: json,
  };

  state.messages += 1;
  state.lastMessageAt = item.ts;
  pushHistory(item);
  broadcast("message", item);
  broadcast("status", state);
});

function sendJson(res, code, body) {
  const text = JSON.stringify(body);
  res.writeHead(code, {
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "no-cache",
  });
  res.end(text);
}

function serveHtml(res) {
  let html = "";
  try {
    html = fs.readFileSync(htmlPath, "utf8");
  } catch (error) {
    sendJson(res, 500, {
      ok: false,
      error: `failed to load ${htmlPath}: ${error.message || String(error)}`,
    });
    return;
  }
  res.writeHead(200, {
    "Content-Type": "text/html; charset=utf-8",
    "Cache-Control": "no-cache",
  });
  res.end(html);
}

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/") {
    serveHtml(res);
    return;
  }

  if (req.method === "GET" && req.url === "/health") {
    sendJson(res, 200, {
      ok: true,
      viewer: "mqtt-live-viewer",
      state,
    });
    return;
  }

  if (req.method === "GET" && req.url === "/events") {
    res.writeHead(200, {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-cache",
      Connection: "keep-alive",
    });
    res.write("retry: 1000\n\n");
    clients.add(res);
    sendSnapshot(res);

    req.on("close", () => {
      clients.delete(res);
      try { res.end(); } catch {}
    });
    return;
  }

  sendJson(res, 404, { ok: false, error: "not found" });
});

server.on("error", (error) => {
  console.error(JSON.stringify({
    ts: new Date().toISOString(),
    level: "error",
    message: "viewer http server failed",
    port: WEB_PORT,
    error: error && error.message ? error.message : String(error),
  }));
  process.exit(1);
});

server.listen(WEB_PORT, "127.0.0.1", () => {
  console.log(
    JSON.stringify({
      ts: new Date().toISOString(),
      level: "info",
      message: "mqtt web viewer started",
      mqttUrl: MQTT_URL,
      topic: MQTT_TOPIC,
      clientId: MQTT_CLIENT_ID,
      webUrl: `http://127.0.0.1:${WEB_PORT}`,
      maxBuffer: MAX_BUFFER,
      reconnectPeriodMs: MQTT_RECONNECT_MS,
      connectTimeoutMs: MQTT_CONNECT_TIMEOUT_MS,
      keepaliveSeconds: MQTT_KEEPALIVE_SECONDS,
      protocolVersion: MQTT_PROTOCOL_VERSION,
    }),
  );
});

function shutdown(signal) {
  console.log(JSON.stringify({
    ts: new Date().toISOString(),
    level: "info",
    message: "shutdown requested",
    signal,
  }));
  try { mqttClient.end(true); } catch {}
  try { server.close(); } catch {}
  setTimeout(() => process.exit(0), 50).unref?.();
}

process.on("SIGINT", () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));
