#!/usr/bin/env node
"use strict";
/*
 * [Input] MQTT_LISTEN_HOST / MQTT_LISTEN_PORT plus bridge-side MQTT clients.
 * [Output] Minimal local MQTT broker for development and Radxa recovery flows.
 * [Pos] script node in scripts.
 * [Sync] If this file changes, update `scripts/.folder.md`.
 */

const net = require("net");
const path = require("path");
const { createRequire } = require("module");

const bridgeRequire = createRequire(path.join(
  __dirname,
  "..",
  "ref",
  "src-tauri",
  "bridge",
  "packages",
  "clawd-backend-service",
  "package.json",
));
const mqttCon = bridgeRequire("mqtt-connection");

const host = process.env.MQTT_LISTEN_HOST || "0.0.0.0";
const port = Number.parseInt(process.env.MQTT_LISTEN_PORT || "1883", 10);
const clients = new Set();
const retained = new Map();

function matchTopic(filter, topic) {
  const f = filter.split("/");
  const t = topic.split("/");
  for (let i = 0; i < f.length; i += 1) {
    if (f[i] === "#") return i === f.length - 1;
    if (i >= t.length) return false;
    if (f[i] !== "+" && f[i] !== t[i]) return false;
  }
  return f.length === t.length;
}

function deliver(packet, from) {
  for (const client of clients) {
    if (!client.subscriptions.some((sub) => matchTopic(sub, packet.topic))) continue;
    client.conn.publish({
      topic: packet.topic,
      payload: packet.payload,
      qos: 0,
      retain: Boolean(packet.retain),
    });
  }
  if (from) {
    console.log(`[mqtt] ${from.id || "client"} -> ${packet.topic}`);
  }
}

const server = net.createServer((stream) => {
  const conn = mqttCon(stream);
  const client = { conn, id: "", subscriptions: [] };
  clients.add(client);

  conn.on("connect", (packet) => {
    client.id = packet.clientId || `client-${Date.now()}`;
    conn.connack({ returnCode: 0 });
    console.log(`[mqtt] client connected: ${client.id}`);
  });

  conn.on("subscribe", (packet) => {
    for (const sub of packet.subscriptions || []) {
      if (sub.topic && !client.subscriptions.includes(sub.topic)) {
        client.subscriptions.push(sub.topic);
      }
    }
    conn.suback({
      messageId: packet.messageId,
      granted: (packet.subscriptions || []).map(() => 0),
    });
    for (const sub of packet.subscriptions || []) {
      for (const retainedPacket of retained.values()) {
        if (matchTopic(sub.topic, retainedPacket.topic)) {
          conn.publish(retainedPacket);
        }
      }
    }
  });

  conn.on("publish", (packet) => {
    if (packet.retain) {
      if (packet.payload && packet.payload.length > 0) retained.set(packet.topic, packet);
      else retained.delete(packet.topic);
    }
    deliver(packet, client);
    if (packet.qos === 1) conn.puback({ messageId: packet.messageId });
  });

  conn.on("pingreq", () => conn.pingresp());
  conn.on("disconnect", () => conn.destroy());
  conn.on("close", () => {
    clients.delete(client);
    console.log(`[mqtt] client disconnected: ${client.id || "unknown"}`);
  });
  conn.on("error", () => {
    clients.delete(client);
  });
});

server.listen(port, host, () => {
  console.log(`[mqtt] broker listening on ${host}:${port}`);
});
