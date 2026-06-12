#!/usr/bin/env node
"use strict";

/**
 * Standalone runner for the bus. Used for local hacking & ad-hoc curl
 * testing — *not* the production embedding (the sidecar embeds the bus
 * via `createAgentSessionBus(...)` from the same Node process).
 *
 * Usage:
 *   AGENT_BUS_PORT=8181 AGENT_BUS_USE_MOCK=1 node src/cli.js
 */

const {
  createAgentSessionBus,
  MockAdapter,
  ClaudeCodeAdapter,
  CodexAdapter,
  OpenClawAdapter,
} = require("./index");
const { defaultLog, withScope } = require("./log");
const { readBool } = require("./env");

async function main() {
  const log = withScope(defaultLog, "cli");
  const adapters = [];

  if (readBool(process.env, "AGENT_BUS_USE_MOCK", false)) {
    adapters.push(new MockAdapter({ agentId: "mock", log: withScope(defaultLog, "mock") }));
  }

  // Real adapters are always registered; isAvailable() reports not-installed
  // cleanly so this is safe on machines that don't have any agent installed.
  // The pet-manager UI uses /agent/status to decide which voice button to
  // enable.
  adapters.push(new ClaudeCodeAdapter({ log: withScope(defaultLog, "claude") }));
  adapters.push(new CodexAdapter({ log: withScope(defaultLog, "codex") }));
  adapters.push(new OpenClawAdapter({ log: withScope(defaultLog, "openclaw") }));

  const bus = createAgentSessionBus({ adapters, log: withScope(defaultLog, "bus") });
  const port = await bus.start();
  log("info", "ready", { port, adapters: adapters.map((a) => a.agentId) });

  const shutdown = async (signal) => {
    log("info", "shutdown requested", { signal });
    try { await bus.stop(); } catch (error) {
      log("warn", "stop threw", { error: String(error?.message || error) });
    }
    process.exit(0);
  };
  process.on("SIGINT", () => shutdown("SIGINT"));
  process.on("SIGTERM", () => shutdown("SIGTERM"));
}

main().catch((error) => {
  defaultLog("error", "cli failed", { error: String(error?.stack || error) });
  process.exit(1);
});
