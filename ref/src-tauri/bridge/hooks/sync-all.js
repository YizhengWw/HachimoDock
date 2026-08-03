"use strict";

const { isMainThread, parentPort, workerData } = require("worker_threads");

function syncAllHooks(options = {}) {
  const outcomes = [];
  const { port, autoStart } = options;

  if (options.syncLegacyHooks !== false) {
    run("claude", () => {
      const { registerHooks } = require("./install.js");
      return registerHooks({ silent: true, autoStart: Boolean(autoStart), port });
    });
    run("gemini", () => {
      const { registerGeminiHooks } = require("./gemini-install.js");
      return registerGeminiHooks({ silent: true });
    });
    run("cursor", () => {
      const { registerCursorHooks } = require("./cursor-install.js");
      return registerCursorHooks({ silent: true });
    });
    run("codebuddy", () => {
      const { registerCodeBuddyHooks } = require("./codebuddy-install.js");
      return registerCodeBuddyHooks({ silent: true });
    });
  }
  if (options.syncMiMoCode === true) {
    run("mimocode", () => {
      const { syncMiMoCodePlugin } = require("./mimocode-install.js");
      return syncMiMoCodePlugin();
    });
  }

  return outcomes;

  function run(name, action) {
    try {
      outcomes.push({ name, result: action() });
    } catch (error) {
      outcomes.push({ name, error: String(error?.message || error) });
    }
  }
}

if (!isMainThread && parentPort) {
  parentPort.postMessage(syncAllHooks(workerData || {}));
}

module.exports = { syncAllHooks };
