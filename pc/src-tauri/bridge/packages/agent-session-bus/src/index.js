"use strict";

const { AgentSessionBus } = require("./server");
const { AdapterRegistry } = require("./registry");
const { BaseAdapter } = require("./adapters/base");
const { MockAdapter } = require("./adapters/mock");
const { ClaudeCodeAdapter } = require("./adapters/claude-code");
const { CodexAdapter } = require("./adapters/codex");
const { OpenClawAdapter } = require("./adapters/openclaw");
const { MiMoCodeAdapter } = require("./adapters/mimocode");
const { defaultLog } = require("./log");
const { readPort } = require("./env");

/**
 * Factory used by callers (the bridge sidecar's main()) to embed the bus.
 *
 * @param {object} opts
 * @param {number}  [opts.port]      override default 8181 / AGENT_BUS_PORT
 * @param {string}  [opts.host]      defaults to 127.0.0.1
 * @param {Array<BaseAdapter>} [opts.adapters]   adapter instances to register
 * @param {(agentId: string) => Array<object>|Promise<Array<object>>} [opts.sessionStatusProvider]
 * @param {(agentId: string, options: object) => object|Promise<object>} [opts.sessionEventProvider]
 * @param {(level: string, msg: string, details?: object) => void} [opts.log]
 * @returns {AgentSessionBus}
 */
function createAgentSessionBus({
  port,
  host = "127.0.0.1",
  adapters = [],
  sessionStatusProvider,
  sessionEventProvider,
  log,
} = {}) {
  const finalLog = log || defaultLog;
  const finalPort = Number.isFinite(port) ? port : readPort(process.env);
  const registry = new AdapterRegistry({ adapters, log: finalLog });
  return new AgentSessionBus({
    port: finalPort,
    host,
    registry,
    sessionStatusProvider,
    sessionEventProvider,
    log: finalLog,
  });
}

module.exports = {
  createAgentSessionBus,
  AgentSessionBus,
  AdapterRegistry,
  BaseAdapter,
  MockAdapter,
  ClaudeCodeAdapter,
  CodexAdapter,
  OpenClawAdapter,
  MiMoCodeAdapter,
};
