/**
 * [Input] A Tauri-generated macOS app/DMG helper plus a stable-local-signed Pet Manager.app.
 * [Output] A replacement local DMG built from that exact app and verified again after read-only mounting.
 * [Pos] Local macOS packaging helper that prevents Tauri's bundle step from replacing the stable TCC signature.
 * [Sync] If this file changes, update `scripts/.folder.md`.
 */

import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  rmSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

if (process.platform !== "darwin") {
  console.error("bundle-macos-local-dmg only supports macOS");
  process.exit(1);
}

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDir, "..");
const bundleRoot = resolve(
  repositoryRoot,
  "ref",
  "src-tauri",
  "target",
  "release",
  "bundle",
);
const appPath = join(bundleRoot, "macos", "Pet Manager.app");
const dmgDirectory = join(bundleRoot, "dmg");
const dmgPath = join(dmgDirectory, "Pet Manager_0.1.0_aarch64.dmg");
const dmgHelper = join(dmgDirectory, "bundle_dmg.sh");
const signer = join(scriptDir, "sign-macos-local-app.mjs");

for (const requiredPath of [appPath, dmgHelper, signer]) {
  if (!existsSync(requiredPath)) {
    console.error(`required local macOS bundle input is missing: ${requiredPath}`);
    process.exit(1);
  }
}

function run(command, args, label) {
  const result = spawnSync(command, args, { stdio: "inherit" });
  if (result.status !== 0) {
    throw new Error(`${label} failed with exit code ${result.status ?? "unknown"}`);
  }
}

function verifyStableApp(path) {
  run(process.execPath, [signer, "--verify-only", path], "stable app signature verification");
}

verifyStableApp(appPath);

mkdirSync(dmgDirectory, { recursive: true });
const stagingRoot = mkdtempSync(join(tmpdir(), "pet-manager-local-dmg-"));
const mountRoot = mkdtempSync(join(tmpdir(), "pet-manager-local-dmg-mount-"));
const stagedAppPath = join(stagingRoot, basename(appPath));
let mounted = false;

try {
  run("/usr/bin/ditto", [appPath, stagedAppPath], "staging stable app");
  verifyStableApp(stagedAppPath);

  rmSync(dmgPath, { force: true });
  run(
    dmgHelper,
    [
      "--volname",
      "Pet Manager",
      "--window-size",
      "500",
      "350",
      "--icon-size",
      "128",
      "--icon",
      "Pet Manager.app",
      "150",
      "170",
      "--hide-extension",
      "Pet Manager.app",
      "--app-drop-link",
      "350",
      "170",
      "--skip-jenkins",
      "--no-internet-enable",
      dmgPath,
      stagingRoot,
    ],
    "stable local DMG creation",
  );

  run(
    "/usr/bin/hdiutil",
    ["attach", "-readonly", "-nobrowse", "-noautoopen", "-mountpoint", mountRoot, dmgPath],
    "mounting final local DMG",
  );
  mounted = true;
  verifyStableApp(join(mountRoot, "Pet Manager.app"));
} finally {
  if (mounted) {
    spawnSync("/usr/bin/hdiutil", ["detach", mountRoot], { stdio: "inherit" });
  }
  rmSync(stagingRoot, { recursive: true, force: true });
  rmSync(mountRoot, { recursive: true, force: true });
}

console.log(`stable local macOS DMG created and verified: ${dmgPath}`);
