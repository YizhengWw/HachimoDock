/**
 * [Input] A locally bundled macOS HachimiDock.app, optionally supplied as a positional argument, plus optional --verify-only.
 * [Output] An ad-hoc signed app whose exact stable bundle-id designated requirement is verified for persistent local TCC grants.
 * [Pos] Local macOS packaging helper; production Developer ID signing remains a separate release concern.
 * [Sync] If this file changes, update `scripts/.folder.md`.
 */

import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { verifyVoiceBundle } from "./voice-runtime-resources.mjs";
import {desktopBuildPaths,macAppIdentity,verifyBuildReceipt,readBuildReceipt,receiptArgument} from './desktop-build-artifact.mjs';

if (process.platform !== "darwin") {
  console.error("sign-macos-local-app only supports macOS");
  process.exit(1);
}

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDir, "../..");
const args = process.argv.slice(2);
const verifyOnly = args.includes("--verify-only");
const receiptPath=receiptArgument(args);
const outputIndex=args.indexOf('--output-receipt');
const outputReceipt=outputIndex<0?null:args[outputIndex+1];
if(outputIndex>=0&&(!outputReceipt||outputReceipt.startsWith('--')||verifyOnly||existsSync(resolve(outputReceipt))))throw Error('invalid output receipt');
const positional=args.filter((arg,i)=>!arg.startsWith('--')&&!['--receipt','--output-receipt'].includes(args[i-1]));
if(positional.length>1||args.some(arg=>arg.startsWith('--')&&!['--verify-only','--receipt','--relocated','--output-receipt'].includes(arg)))throw Error('invalid signer arguments');
const paths=desktopBuildPaths({repositoryRoot,target:'macos'});
const appPath=resolve(positional[0]||resolve(paths.bundleRoot,'macos/HachimiDock.app'));
const version=JSON.parse(readFileSync(resolve(repositoryRoot,'pc/src-tauri/tauri.conf.json'),'utf8')).version;
const assertIdentity=()=>{
  const identity=macAppIdentity(appPath,version);
  if(receiptPath)verifyBuildReceipt(identity,readBuildReceipt(receiptPath),{relocated:args.includes('--relocated')});
  return identity;
};
assertIdentity(); // reject wrong version/hash before modifying any signature

if (!existsSync(appPath)) {
  console.error(`Pet Manager app bundle not found: ${appPath}`);
  process.exit(1);
}

// Verify install-relative service code/dependencies before either sign or verify.
console.log(JSON.stringify({voiceRuntime:verifyVoiceBundle(resolve(appPath,"Contents/Resources"),{target:"macos",arch:process.arch})}));

const designatedRequirement =
  '=designated => identifier "com.petmanager.desktop"';
if (!verifyOnly) {
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
}

const verified = spawnSync(
  "codesign",
  ["--verify", "--deep", "--strict", "--verbose=2", appPath],
  { stdio: "inherit" },
);
if (verified.status !== 0) {
  process.exit(verified.status || 1);
}

const inspected = spawnSync(
  "codesign",
  ["--display", "--requirements", "-", appPath],
  { encoding: "utf8" },
);
const inspectionOutput = `${inspected.stdout || ""}\n${inspected.stderr || ""}`;
if (
  inspected.status !== 0
  || !inspectionOutput.includes('designated => identifier "com.petmanager.desktop"')
) {
  console.error(`stable local designated requirement is missing: ${appPath}`);
  process.exit(inspected.status || 1);
}

// codesign is expected to change the embedded Mach-O signature. The incoming
// receipt was checked before signing; retain it and emit a fresh signed identity
// for subsequent copy/DMG verification, never overwrite the build receipt.
const signedIdentity=macAppIdentity(appPath,version);
if(verifyOnly)assertIdentity();
if(outputReceipt)writeFileSync(resolve(outputReceipt),JSON.stringify(signedIdentity,null,2)+'\n',{flag:'wx'});

console.log(`${verifyOnly ? "verified" : "applied"} stable local macOS signature: ${appPath}`);
