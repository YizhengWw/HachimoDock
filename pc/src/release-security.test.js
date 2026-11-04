import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import {createHash} from "node:crypto";
import {dependencyLicenseResources} from "../scripts/dependency-license-resources.mjs";
const read = p => fs.readFileSync(new URL(p, import.meta.url), "utf8");
test("Tauri lock includes the IPC origin security fix", () => {
  const lock=read("../src-tauri/Cargo.lock");
  const version=lock.match(/name = "tauri"\nversion = "(\d+)\.(\d+)\.(\d+)"/);
  assert.ok(version);
  const [major,minor,patch]=version.slice(1).map(Number);
  assert.ok(major>2 || (major===2 && (minor>11 || (minor===11 && patch>=1))));
});
test("both desktop packages preserve ESP-SR license", () => {
  const config=JSON.parse(read("../src-tauri/tauri.conf.json"));
  assert.equal(config.bundle.resources["../../licenses/Espressif-ESP-SR-LICENSE.txt"],"licenses/Espressif-ESP-SR-LICENSE.txt");
  const selected=dependencyLicenseResources.find(x=>x.name==="Espressif-ESP-SR-LICENSE.txt");
  assert.ok(selected, "ESP-SR must remain on the verified license allowlist");
  const bytes=fs.readFileSync(new URL("../../licenses/Espressif-ESP-SR-LICENSE.txt", import.meta.url));
  assert.equal(createHash("sha256").update(bytes).digest("hex"), selected.sha256);
  const windows=read("../scripts/build-windows-nsis-cross.mjs");
  assert.match(windows,/import\s*\{[^}]*\bcopyDependencyLicenses\b[^}]*\}\s*from\s*["']\.\/dependency-license-resources\.mjs["']/);
  assert.match(windows,/copyDependencyLicenses\(join\(repositoryRoot,\s*["']licenses["']\),\s*join\(stageRoot,\s*["']licenses["']\)\)/);
});
