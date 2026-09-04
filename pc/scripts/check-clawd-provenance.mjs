/**
 * [Input] Audited MIT-era Clawd bridge files plus repository provenance and notice documents.
 * [Output] A failing release gate when retained upstream bytes, required notices, or retired wrappers drift without review.
 * [Pos] Commercial-release provenance guard for desktop source validation.
 * [Sync] If this file changes, update `scripts/.folder.md` and `docs/clawd-mit-provenance.md`.
 */

import { createHash } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDir, "../..");

const auditedFiles = new Map([
  [
    "pc/src-tauri/bridge/hooks/auto-start.js",
    "57d660b5d766b3f0d2e071462a52da3cdbaf5490e75b61d00b25b2d0b3935570",
  ],
  [
    "pc/src-tauri/bridge/hooks/codebuddy-install.js",
    "e299f35a0c7998040d30dc46ddc2d562154bff1b41506adc7bad24097ffde812",
  ],
  [
    "pc/src-tauri/bridge/hooks/cursor-install.js",
    "af382e661917da86ca4329826139414ff6c109761a59d602f8d209d76d5e8390",
  ],
  [
    "pc/src-tauri/bridge/hooks/gemini-install.js",
    "134a409628af84acd0e891aae3336f5e35039b1b3839d96f0af2fd0f8c6055a1",
  ],
  [
    "pc/src-tauri/bridge/hooks/server-config.js",
    "21f2546c3e9e367b30de66b668fdcde7bf7db5cc08f52d68765e5d51e237761b",
  ],
]);

const retiredWrappers = [
  "pc/src-tauri/bridge/agents/codebuddy.js",
  "pc/src-tauri/bridge/agents/copilot-cli.js",
  "pc/src-tauri/bridge/agents/cursor-agent.js",
  "pc/src-tauri/bridge/agents/gemini-cli.js",
  "pc/src-tauri/bridge/agents/registry.js",
];

function fail(message) {
  console.error(`Clawd provenance check failed: ${message}`);
  process.exitCode = 1;
}

for (const [relativePath, expectedSha256] of auditedFiles) {
  const absolutePath = resolve(repositoryRoot, relativePath);
  if (!existsSync(absolutePath)) {
    fail(`audited MIT-era file is missing: ${relativePath}`);
    continue;
  }
  const actualSha256 = createHash("sha256")
    .update(readFileSync(absolutePath))
    .digest("hex");
  if (actualSha256 !== expectedSha256) {
    fail(
      `${relativePath} changed (${actualSha256}); review its upstream provenance and update the audit before accepting the change`,
    );
  }
}

for (const relativePath of retiredWrappers) {
  if (existsSync(resolve(repositoryRoot, relativePath))) {
    fail(`retired unreachable wrapper was restored: ${relativePath}`);
  }
}

const noticePath = resolve(repositoryRoot, "THIRD_PARTY_NOTICES.md");
const auditPath = resolve(repositoryRoot, "pc", "docs", "clawd-mit-provenance.md");
if (!existsSync(noticePath)) {
  fail("THIRD_PARTY_NOTICES.md is missing");
} else {
  const notice = readFileSync(noticePath, "utf8");
  for (const requiredText of [
    "Copyright (c) 2026 rullerzhou-afk",
    "Permission is hereby granted, free of charge",
    "THE SOFTWARE IS PROVIDED \"AS IS\"",
  ]) {
    if (!notice.includes(requiredText)) {
      fail(`THIRD_PARTY_NOTICES.md is missing required MIT text: ${requiredText}`);
    }
  }
}

if (!existsSync(auditPath)) {
  fail("docs/clawd-mit-provenance.md is missing");
} else {
  const audit = readFileSync(auditPath, "utf8");
  for (const requiredText of [
    "19e8f82493b0993554df62b3eba419b1127fff14",
    "3b6277ff39b4473bd0b0a09d55a695b176c815e9",
  ]) {
    if (!audit.includes(requiredText)) {
      fail(`Clawd provenance audit is missing boundary commit: ${requiredText}`);
    }
  }
}

if (process.exitCode) process.exit(process.exitCode);
console.log(`Clawd MIT provenance verified for ${auditedFiles.size} retained upstream files`);
