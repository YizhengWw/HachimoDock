/**
 * [Input] A locally bundled macOS Pet Manager.app, optionally supplied as argv[2].
 * [Output] An ad-hoc signed app with a stable bundle-id designated requirement for persistent local TCC grants.
 * [Pos] Local macOS packaging helper; production Developer ID signing remains a separate release concern.
 * [Sync] If this file changes, update `scripts/.folder.md`.
 */

import { existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

if (process.platform !== "darwin") {
  console.error("sign-macos-local-app only supports macOS");
  process.exit(1);
}

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDir, "..");
const appPath = resolve(
  process.argv[2]
    || resolve(
      repositoryRoot,
      "ref",
      "src-tauri",
      "target",
      "release",
      "bundle",
      "macos",
      "Pet Manager.app",
    ),
);
if (!existsSync(appPath)) {
  console.error(`Pet Manager app bundle not found: ${appPath}`);
  process.exit(1);
}

const designatedRequirement =
  '=designated => identifier "com.petmanager.desktop"';
const signed = spawnSync(
  "codesign",
  [
    "--force",
    "--options",
    "runtime",
    "--sign",
    "-",
    "--requirements",
    designatedRequirement,
    appPath,
  ],
  { stdio: "inherit" },
);
if (signed.status !== 0) {
  process.exit(signed.status || 1);
}

const verified = spawnSync(
  "codesign",
  ["--verify", "--deep", "--strict", "--verbose=2", appPath],
  { stdio: "inherit" },
);
if (verified.status !== 0) {
  process.exit(verified.status || 1);
}

console.log(`stable local macOS signature applied: ${appPath}`);
