"use strict";

/*
 * [Input] Managed bridge parent-process environment.
 * [Output] Regression coverage for orphan Bridge shutdown after desktop exit.
 * [Pos] Lifecycle tests for the headless MQTT sidecar.
 */

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { test } = require("node:test");

const { startParentProcessWatchdog } = require(
  "../packages/clawd-backend-service/src/headless-mqtt",
);

test("managed bridge notices when its desktop parent no longer exists", async () => {
  const previous = process.env.PET_MANAGER_PARENT_PID;
  process.env.PET_MANAGER_PARENT_PID = "2147483646";
  try {
    const parentPid = await new Promise((resolve, reject) => {
      const timeout = setTimeout(
        () => reject(new Error("parent watchdog did not fire")),
        2500,
      );
      const watchdog = startParentProcessWatchdog((parentPid) => {
        clearTimeout(timeout);
        watchdog.stop();
        resolve(parentPid);
      });
    });

    assert.equal(parentPid, 2147483646);
  } finally {
    if (previous === undefined) {
      delete process.env.PET_MANAGER_PARENT_PID;
    } else {
      process.env.PET_MANAGER_PARENT_PID = previous;
    }
  }
});

test("managed bridge fails closed when its embedded Agent Bus cannot start", () => {
  const source = fs.readFileSync(
    path.join(
      __dirname,
      "..",
      "packages",
      "clawd-backend-service",
      "src",
      "headless-mqtt.js",
    ),
    "utf8",
  );

  assert.match(source, /if \(!busModule\) \{[\s\S]*throw new Error\("agent-session-bus is required/);
  assert.match(source, /agent-session-bus failed to start[\s\S]*agentBus = null;[\s\S]*throw error;/);
});
