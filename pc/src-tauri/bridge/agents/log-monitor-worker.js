"use strict";

const { isMainThread, parentPort, workerData } = require("worker_threads");

if (isMainThread || !parentPort) {
  throw new Error("log-monitor-worker must run in a worker thread");
}

const definitions = {
  codex: "./codex-log-monitor",
  claude: "./claude-log-monitor",
};
const kind = String(workerData?.kind || "");
const modulePath = definitions[kind];
if (!modulePath) {
  throw new Error(`unsupported log monitor kind: ${kind}`);
}

const LogMonitor = require(modulePath);
const monitor = new LogMonitor({}, (sessionId, state, event, extra) => {
  parentPort.postMessage({
    type: "state",
    sessionId,
    state,
    event,
    extra,
  });
});

monitor.start();
parentPort.postMessage({ type: "ready", kind });
parentPort.once("message", (message) => {
  if (message?.type !== "stop") return;
  monitor.stop();
  parentPort.close();
});
