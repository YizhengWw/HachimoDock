/*
 * [Input] Headless Bridge atomic snapshot helper and local state publishers.
 * [Output] Regression coverage for durable same-directory replace semantics and complete JSON snapshots.
 * [Pos] Bridge host-side regression test.
 */

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const {
  MqttPublisher,
  writeSnapshotAtomicSync,
} = require("../packages/clawd-backend-service/src/headless-mqtt");

function stagingFiles(directory) {
  return fs.readdirSync(directory).filter((name) => name.endsWith(".tmp"));
}

test("atomic snapshot writer replaces complete JSON and removes staging files", () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "pet-bridge-atomic-"));
  const target = path.join(directory, "codex.json");
  try {
    writeSnapshotAtomicSync(target, '{"revision":1,"body":"old"}\n');
    writeSnapshotAtomicSync(target, '{"revision":2,"body":"new"}\n');

    assert.deepEqual(JSON.parse(fs.readFileSync(target, "utf8")), {
      revision: 2,
      body: "new",
    });
    assert.deepEqual(stagingFiles(directory), []);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

test("state and speech publishers atomically commit both latest and per-session files", () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "pet-bridge-publisher-"));
  const stateDir = path.join(root, "state");
  const speechDir = path.join(root, "speech");
  fs.mkdirSync(speechDir, { recursive: true });
  const publisher = new MqttPublisher({
    namespace: "desk",
    deviceId: "test-device",
    qos: 0,
    retain: false,
    localStateDir: stateDir,
  });
  publisher._localSpeechDir = speechDir;
  const payload = {
    source: "codex",
    sessionId: "session-atomic",
    state: "working",
    text: "complete snapshot",
  };

  try {
    publisher._writeLocalState("codex", payload);
    publisher._writeLocalSpeech("codex", payload);

    for (const directory of [stateDir, speechDir]) {
      const snapshots = fs.readdirSync(directory).filter((name) => name.endsWith(".json"));
      assert.equal(snapshots.length, 2);
      for (const name of snapshots) {
        assert.deepEqual(JSON.parse(fs.readFileSync(path.join(directory, name), "utf8")), payload);
      }
      assert.deepEqual(stagingFiles(directory), []);
    }
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("atomic helper fsyncs the staged file before replacing the target", () => {
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
  const helper = source.match(/function writeSnapshotAtomicSync[\s\S]*?\r?\n}\r?\n/);
  assert.ok(helper);
  assert.match(helper[0], /fs\.openSync\(tempPath, "wx"/);
  assert.match(helper[0], /fs\.writeFileSync\(descriptor[\s\S]*fs\.fsyncSync\(descriptor\)[\s\S]*fs\.closeSync\(descriptor\)[\s\S]*fs\.renameSync\(tempPath, filePath\)/);
  assert.doesNotMatch(helper[0], /unlinkSync\(filePath\)/);
});
