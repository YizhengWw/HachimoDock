#!/usr/bin/env node
/*
 * [Input] Consume MQTT broker config from env or sibling pet-claw/.env plus CLI watch options.
 * [Output] Stream remote-cli control messages and followed bridge state topics for Pet Manager debugging.
 * [Pos] script node in scripts
 * [Sync] If this file changes, update `scripts/.folder.md`.
 */

const fs = require('fs');
const path = require('path');
const { createRequire } = require('module');

const SCRIPT_DIR = __dirname;
const WORKSPACE_ROOT = path.resolve(SCRIPT_DIR, '..');
const PET_CLAW_ROOT = path.resolve(WORKSPACE_ROOT, '..', 'pet-claw');
const PET_CLAW_ENV_PATH = path.join(PET_CLAW_ROOT, '.env');

function parseArgs(argv) {
  const options = {
    linuxDevice: process.env.LINUX_PET_DEVICE_ID || 'linux-pet',
    targetDevice: process.env.TARGET_DEVICE_ID || '',
    namespace: process.env.STATUS_NAMESPACE || process.env.PET_CLAW_MQTT_NAMESPACE || 'desk',
    durationSeconds: 0,
    raw: false,
  };

  for (let index = 0; index < argv.length; index += 1) {
    const arg = argv[index];
    const readNext = () => argv[++index] || '';
    if (arg === '--linux-device' || arg === '--recipient-device') options.linuxDevice = readNext();
    else if (arg === '--target-device' || arg === '--source-device') options.targetDevice = readNext();
    else if (arg === '--namespace') options.namespace = readNext();
    else if (arg === '--url') options.url = readNext();
    else if (arg === '--username') options.username = readNext();
    else if (arg === '--password') options.password = readNext();
    else if (arg === '--duration') options.durationSeconds = Number(readNext()) || 0;
    else if (arg === '--raw') options.raw = true;
    else if (arg === '--help' || arg === '-h') options.help = true;
  }

  return options;
}

function printHelp() {
  console.log(`Usage:
  node scripts/watch-remote-cli-mqtt.cjs [options]

Options:
  --linux-device <id>     Linux pet-claw device id to watch. Default: linux-pet
  --target-device <id>    Bridge source device id to watch immediately. Optional.
  --namespace <name>      MQTT namespace. Default: desk or STATUS_NAMESPACE
  --url <mqtt-url>        MQTT broker URL. Falls back to env or ../pet-claw/.env
  --username <value>      MQTT username. Optional.
  --password <value>      MQTT password. Optional.
  --duration <seconds>    Stop after N seconds. Default: run until Ctrl-C.
  --raw                   Print raw JSON payloads.
`);
}

function readDotenv(filePath) {
  if (!fs.existsSync(filePath)) return {};
  const result = {};
  const content = fs.readFileSync(filePath, 'utf8');
  for (const line of content.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('#')) continue;
    const normalized = trimmed.startsWith('export ') ? trimmed.slice(7).trimStart() : trimmed;
    const eqIndex = normalized.indexOf('=');
    if (eqIndex < 0) continue;
    const key = normalized.slice(0, eqIndex).trim();
    let value = normalized.slice(eqIndex + 1).trim();
    if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith("'") && value.endsWith("'"))) {
      value = value.slice(1, -1);
    }
    result[key] = value;
  }
  return result;
}

function resolveConfig(options) {
  const dotenv = readDotenv(PET_CLAW_ENV_PATH);
  const getValue = (...names) => {
    for (const name of names) {
      const value = options[name] || process.env[name] || dotenv[name];
      if (typeof value === 'string' && value.trim()) return value.trim();
    }
    return '';
  };

  return {
    url: options.url || getValue('PET_CLAW_MQTT_URL', 'MQTT_URL') || 'mqtt://127.0.0.1:1883',
    username: options.username || getValue('PET_CLAW_MQTT_USERNAME', 'MQTT_USERNAME'),
    password: options.password || getValue('PET_CLAW_MQTT_PASSWORD', 'MQTT_PASSWORD'),
  };
}

function loadMqtt() {
  try {
    return require('mqtt');
  } catch {}

  const petClawPackage = path.join(PET_CLAW_ROOT, 'package.json');
  if (fs.existsSync(petClawPackage)) {
    return createRequire(petClawPackage)('mqtt');
  }

  throw new Error('Cannot load mqtt. Run npm install in pet-claw, or run this from a workspace with mqtt installed.');
}

function normalizeTopicPart(value, fallback = '') {
  const normalized = String(value || '')
    .trim()
    .replace(/[^a-zA-Z0-9._-]+/g, '-')
    .replace(/-+/g, '-')
    .replace(/^-+|-+$/g, '');
  return normalized || fallback;
}

function parseJson(buffer) {
  const text = buffer.toString('utf8');
  try {
    return { payload: JSON.parse(text), text };
  } catch {
    return { payload: null, text };
  }
}

function nowLabel() {
  return new Date().toISOString();
}

function summarizeState(topic, payload, packet) {
  return {
    topic,
    retain: Boolean(packet.retain),
    source: payload?.source || '',
    state: payload?.state || '',
    event: payload?.event || '',
    reason: payload?.reason || '',
    sessionId: payload?.sessionId || '',
    tsMs: payload?.tsMs || 0,
  };
}

function summarizeControl(topic, payload, packet) {
  return {
    topic,
    retain: Boolean(packet.retain),
    command: payload?.command || '',
    commandId: payload?.commandId || '',
    recipientDeviceId: payload?.recipientDeviceId || '',
    enabled: Boolean(payload?.enabled),
    targetDeviceId: payload?.targetDeviceId || '',
    targetSource: payload?.targetSource || '',
    mqttNamespace: payload?.mqttNamespace || '',
    updatedAt: payload?.updatedAt || 0,
    updatedBy: payload?.updatedBy || '',
  };
}

function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.help) {
    printHelp();
    return;
  }

  const mqtt = loadMqtt();
  const config = resolveConfig(options);
  const namespace = normalizeTopicPart(options.namespace, 'desk');
  const linuxDevice = normalizeTopicPart(options.linuxDevice, 'linux-pet');
  const controlTopic = `${namespace}/${linuxDevice}/control/remote-cli-binding`;
  const watchedTargets = new Set();

  const client = mqtt.connect(config.url, {
    clientId: `pet-manager-remote-cli-watch-${Date.now()}`,
    username: config.username || undefined,
    password: config.password || undefined,
    protocolVersion: 5,
    reconnectPeriod: 1500,
    connectTimeout: 8000,
  });

  const watchTarget = (targetDevice, reason) => {
    const normalizedTarget = normalizeTopicPart(targetDevice, '');
    if (!normalizedTarget || watchedTargets.has(normalizedTarget)) return;
    watchedTargets.add(normalizedTarget);
    const topics = [
      `${namespace}/${normalizedTarget}/state/+`,
      `${namespace}/${normalizedTarget}/speech/text`,
      `${namespace}/${normalizedTarget}/availability/bridge`,
    ];
    client.subscribe(topics, { qos: 1 }, (error) => {
      if (error) {
        console.error(`[${nowLabel()}] subscribe target failed`, { targetDevice: normalizedTarget, error: error.message });
        return;
      }
      console.log(`[${nowLabel()}] watching target ${normalizedTarget} (${reason})`);
      for (const topic of topics) console.log(`  ${topic}`);
    });
  };

  client.on('connect', () => {
    console.log(`[${nowLabel()}] connected ${config.url}`);
    client.subscribe(controlTopic, { qos: 1 }, (error) => {
      if (error) {
        console.error(`[${nowLabel()}] subscribe control failed`, error.message);
        return;
      }
      console.log(`[${nowLabel()}] watching control ${controlTopic}`);
    });
    if (options.targetDevice) watchTarget(options.targetDevice, 'cli');
  });

  client.on('message', (topic, buffer, packet) => {
    const { payload, text } = parseJson(buffer);
    if (topic === controlTopic) {
      const summary = summarizeControl(topic, payload, packet);
      console.log(`[${nowLabel()}] CONTROL ${JSON.stringify(summary)}`);
      if (options.raw) console.log(text);
      if (summary.enabled && summary.targetDeviceId) watchTarget(summary.targetDeviceId, `control ${summary.commandId || 'retained'}`);
      return;
    }

    if (topic.includes('/state/')) {
      console.log(`[${nowLabel()}] STATE ${JSON.stringify(summarizeState(topic, payload, packet))}`);
      if (options.raw) console.log(text);
      return;
    }

    if (topic.includes('/availability/')) {
      console.log(`[${nowLabel()}] AVAIL ${JSON.stringify({
        topic,
        retain: Boolean(packet.retain),
        source: payload?.source || '',
        online: Boolean(payload?.online),
        tsMs: payload?.tsMs || 0,
      })}`);
      if (options.raw) console.log(text);
      return;
    }

    console.log(`[${nowLabel()}] MESSAGE ${topic} ${text}`);
  });

  client.on('error', (error) => {
    console.error(`[${nowLabel()}] mqtt error`, error.message);
  });

  const stop = () => {
    console.log(`[${nowLabel()}] stopping watcher`);
    client.end(true, () => process.exit(0));
  };

  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
  if (options.durationSeconds > 0) setTimeout(stop, options.durationSeconds * 1000);
}

main();
