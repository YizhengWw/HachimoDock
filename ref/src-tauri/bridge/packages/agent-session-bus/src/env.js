"use strict";

const DEFAULT_PORT = 8181;

function readPort(env) {
  const raw = env && env.AGENT_BUS_PORT;
  if (typeof raw !== "string" || raw.trim() === "") return DEFAULT_PORT;
  const value = Number.parseInt(raw, 10);
  return Number.isFinite(value) && value > 0 && value < 65536 ? value : DEFAULT_PORT;
}

function readBool(env, name, fallback = false) {
  const raw = env && env[name];
  if (typeof raw !== "string" || raw.trim() === "") return fallback;
  const v = raw.trim().toLowerCase();
  if (["1", "true", "yes", "on"].includes(v)) return true;
  if (["0", "false", "no", "off"].includes(v)) return false;
  return fallback;
}

module.exports = {
  DEFAULT_PORT,
  readPort,
  readBool,
};
