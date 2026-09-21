"use strict";

function nowIso() {
  return new Date().toISOString();
}

function defaultLog(level, message, details) {
  const payload = {
    ts: nowIso(),
    level,
    component: "agent-session-bus",
    message,
    ...(details && typeof details === "object" ? details : {}),
  };
  const line = JSON.stringify(payload);
  if (level === "error") console.error(line);
  else console.log(line);
}

function withScope(log, scope) {
  return (level, message, details) => {
    log(level, `${scope} :: ${message}`, details);
  };
}

module.exports = {
  defaultLog,
  withScope,
};
