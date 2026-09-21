/**
 * [Input] Tauri product/version configuration, native macOS architecture, optional validated build flavor, a generated DMG helper, and a stable-local-signed app.
 * [Output] A config-versioned replacement local DMG, optionally flavor-suffixed, built from that exact app and verified again after read-only mounting.
 * [Pos] Local macOS packaging helper that prevents Tauri's bundle step from replacing the stable TCC signature.
 * [Sync] If this file changes, update `scripts/.folder.md`.
 */

import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
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
const repositoryRoot = resolve(scriptDir, "../..");
const tauriConfigPath = resolve(repositoryRoot, "pc", "src-tauri", "tauri.conf.json");
const tauriConfig = JSON.parse(readFileSync(tauriConfigPath, "utf8"));
const productName = String(tauriConfig.productName || "").trim();
const appVersion = String(tauriConfig.version || "").trim();
const dmgFlavor = String(process.env.PET_MANAGER_DMG_FLAVOR || "").trim();
const dmgArchitecture = process.arch === "arm64"
  ? "aarch64"
  : process.arch === "x64"
    ? "x64"
    : "";
if (!productName || !/^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$/.test(appVersion)) {
  console.error(`invalid Tauri product/version configuration: ${tauriConfigPath}`);
  process.exit(1);
}
if (!dmgArchitecture) {
  console.error(`unsupported macOS packaging architecture: ${process.arch}`);
  process.exit(1);
}
if (dmgFlavor && !/^[0-9A-Za-z][0-9A-Za-z.-]*$/.test(dmgFlavor)) {
  console.error(`invalid PET_MANAGER_DMG_FLAVOR: ${dmgFlavor}`);
  process.exit(1);
}
const bundleRoot = resolve(
  repositoryRoot,
  "pc",
  "src-tauri",
  "target",
  "release",
  "bundle",
);
const appBundleName = `${productName}.app`;
const appPath = join(bundleRoot, "macos", appBundleName);
const dmgDirectory = join(bundleRoot, "dmg");
const dmgFlavorSuffix = dmgFlavor ? `_${dmgFlavor}` : "";
const dmgPath = join(
  dmgDirectory,
  `${productName}_${appVersion}${dmgFlavorSuffix}_${dmgArchitecture}.dmg`,
);
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
      productName,
      "--window-size",
      "500",
      "350",
      "--icon-size",
      "128",
      "--icon",
      appBundleName,
      "150",
      "170",
      "--hide-extension",
      appBundleName,
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
  verifyStableApp(join(mountRoot, appBundleName));
} finally {
  if (mounted) {
    spawnSync("/usr/bin/hdiutil", ["detach", mountRoot], { stdio: "inherit" });
  }
  rmSync(stagingRoot, { recursive: true, force: true });
  rmSync(mountRoot, { recursive: true, force: true });
}

console.log(`stable local macOS DMG created and verified: ${dmgPath}`);
