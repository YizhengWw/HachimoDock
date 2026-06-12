"use strict";

const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { findExecutable, compareVersions, _internal } = require("../src/util/binary-resolver");

function makeFakeBin(dir, name) {
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, name);
  fs.writeFileSync(file, "#!/bin/sh\nexit 0\n", { mode: 0o755 });
  return file;
}

function withTmp(fn) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "bus-resolver-test-"));
  try {
    return fn(root);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

test("compareVersions handles standard cases", () => {
  assert.equal(compareVersions("1.0.0", "1.0.0"), 0);
  assert.equal(compareVersions("1.2.3", "1.2.4"), -1);
  assert.equal(compareVersions("2.0.0", "1.99.99"), 1);
  assert.equal(compareVersions("1.10", "1.2"), 1);
  assert.equal(compareVersions("1.2", "1.2.0"), 0);
});

test("env var override is honoured if it points to a real executable", () => {
  withTmp((dir) => {
    const bin = makeFakeBin(path.join(dir, "custom"), "claude");
    const found = findExecutable({
      binName: "claude",
      envVar: "CLAUDE_CLI_PATH",
      fallbackPaths: [],
      extraPathDirs: [],
      env: { CLAUDE_CLI_PATH: bin, HOME: dir, PATH: "" },
      platform: "linux",
    });
    assert.equal(found, bin);
  });
});

test("env var override is ignored when path does not exist", () => {
  withTmp((dir) => {
    const fallback = makeFakeBin(path.join(dir, "fallback"), "claude");
    const found = findExecutable({
      binName: "claude",
      envVar: "CLAUDE_CLI_PATH",
      fallbackPaths: [fallback],
      env: { CLAUDE_CLI_PATH: "/no/such/file", HOME: dir, PATH: "" },
      platform: "linux",
    });
    assert.equal(found, fallback);
  });
});

test("PATH is searched after extraPathDirs are prepended", () => {
  withTmp((dir) => {
    const extra = path.join(dir, "extra");
    const onPath = path.join(dir, "onpath");
    const a = makeFakeBin(extra, "codex");
    const b = makeFakeBin(onPath, "codex");
    const found = findExecutable({
      binName: "codex",
      env: { HOME: dir, PATH: onPath },
      extraPathDirs: [extra],
      platform: "linux",
    });
    // extra wins because it's prepended first
    assert.equal(found, a);
    // sanity: the PATH-only one would also be a valid hit
    assert.ok(b);
  });
});

test("fallback path with ~ is expanded against HOME", () => {
  withTmp((dir) => {
    const bin = makeFakeBin(path.join(dir, ".npm-global", "bin"), "claude");
    const found = findExecutable({
      binName: "claude",
      env: { HOME: dir, PATH: "" },
      fallbackPaths: ["~/.npm-global/bin/claude"],
      platform: "linux",
    });
    assert.equal(found, bin);
  });
});

test("returns null when nothing matches", () => {
  withTmp((dir) => {
    const found = findExecutable({
      binName: "claude",
      env: { HOME: dir, PATH: "" },
      fallbackPaths: ["~/this/never/exists"],
      platform: "linux",
    });
    assert.equal(found, null);
  });
});

test("non-executable file in fallback path is rejected", () => {
  withTmp((dir) => {
    const file = path.join(dir, "claude");
    fs.writeFileSync(file, "not a script");
    fs.chmodSync(file, 0o644);
    const found = findExecutable({
      binName: "claude",
      env: { HOME: dir, PATH: "" },
      fallbackPaths: [file],
      platform: "linux",
    });
    assert.equal(found, null);
  });
});

test("augmentPath dedupes and preserves order", () => {
  const out = _internal.augmentPath("/usr/bin:/usr/local/bin", ["/opt/homebrew/bin", "/usr/bin"], "/home/x", "linux");
  // extras come first, then PATH; duplicates suppressed
  assert.equal(out, "/opt/homebrew/bin:/usr/bin:/usr/local/bin");
});

test("Windows PATH lookup honours PATHEXT without requiring unix executable bits", () => {
  withTmp((dir) => {
    const binDir = path.join(dir, "AppData", "Roaming", "npm");
    fs.mkdirSync(binDir, { recursive: true });
    const cmd = path.join(binDir, "codex.CMD");
    fs.writeFileSync(cmd, "@echo off\r\nexit /b 0\r\n", { mode: 0o644 });
    const found = findExecutable({
      binName: "codex",
      env: {
        USERPROFILE: dir,
        PATH: binDir,
        PATHEXT: ".COM;.EXE;.BAT;.CMD",
      },
      fallbackPaths: [],
      extraPathDirs: [],
      platform: "win32",
    });
    assert.equal(found, cmd);
  });
});

test("Windows lookup does not append PATHEXT when binName already has an extension", () => {
  withTmp((dir) => {
    const winApps = path.join(dir, "AppData", "Local", "Microsoft", "WindowsApps");
    fs.mkdirSync(winApps, { recursive: true });
    const exe = path.join(winApps, "claude.exe");
    fs.writeFileSync(exe, "fake exe", { mode: 0o644 });
    const found = findExecutable({
      binName: "claude.exe",
      env: {
        USERPROFILE: dir,
        PATH: winApps,
        PATHEXT: ".EXE;.CMD",
      },
      fallbackPaths: [],
      extraPathDirs: [],
      platform: "win32",
    });
    assert.equal(found, exe);
  });
});
