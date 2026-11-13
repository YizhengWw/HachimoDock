'use strict'

// Entry point: resolve the Hermes home and bridge port, then start the
// read-only state poller. Configuration is environment-only; there is no
// daemon file, no Hermes-side hook, no firmware change.

const path = require('node:path');
const os = require('node:os');
const fs = require('node:fs');
const { HermesMonitor } = require('./hermes-monitor');

function resolvePaths() {
  const hermesHome = process.env.HERMES_HOME || path.join(os.homedir(), '.hermes');
  const clawdHome = process.env.CLAWD_HOME || path.join(os.homedir(), '.clawd');
  return {
    stateDbPath: process.env.HERMES_STATE_DB || path.join(hermesHome, 'state.db'),
    runtimeJsonPath: process.env.CLAWD_RUNTIME_JSON || path.join(clawdHome, 'runtime.json'),
    lockPath: process.env.HERMES_LOCK_FILE || path.join(clawdHome, 'hermes-hachimodock-lock.json'),
    watermarkPath: process.env.HERMES_WATERMARK_FILE || path.join(clawdHome, 'hermes-hachimodock-watermark.json'),
  };
}

async function main() {
  const paths = resolvePaths();
  const monitor = new HermesMonitor({
    ...paths,
    port: Number(process.env.HACHIMODOCK_STATE_PORT) || null,
    pollIntervalMs: Number(process.env.HERMES_POLL_MS) || 1000,
    log: (level, message, details) => console.log(JSON.stringify({ level, message, ...details })),
  });
  const stop = () => { monitor.stop(); process.exit(0); };
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
  const payload = await monitor.pollOnce();
  if (payload) console.log('first state posted:', JSON.stringify(payload));
  monitor.start();
}

if (require.main === module) {
  main().catch((error) => { console.error('hermes-hachimodock failed to start:', error); process.exit(1); });
}

module.exports = { main, resolvePaths, HermesMonitor };
