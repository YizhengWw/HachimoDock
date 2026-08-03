/**
 * [Input] JavaScript assets emitted by the production Vite build.
 * [Output] A failing exit status when eval or Function-constructor execution would violate the release CSP.
 * [Pos] Release build guard shared by desktop targets.
 * [Sync] If this file changes, update scripts/.folder.md and ref/package.json.
 */

import { readdir, readFile } from "node:fs/promises";
import { extname, resolve } from "node:path";

const distRoot = resolve(process.cwd(), process.argv[2] || "dist");
const executableExtensions = new Set([".js", ".mjs"]);
const forbidden = [
  { label: "Function constructor", pattern: /(?:^|[^\w$])(?:new\s+)?Function\s*\(/g },
  { label: "eval", pattern: /(?:^|[^\w$])eval\s*\(/g },
];

async function collectExecutableAssets(directory) {
  const entries = await readdir(directory, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const path = resolve(directory, entry.name);
    if (entry.isDirectory()) files.push(...await collectExecutableAssets(path));
    else if (entry.isFile() && executableExtensions.has(extname(entry.name))) files.push(path);
  }
  return files;
}

const assets = await collectExecutableAssets(distRoot);
if (assets.length === 0) throw new Error(`No production JavaScript assets found under ${distRoot}`);

const failures = [];
for (const path of assets) {
  const source = await readFile(path, "utf8");
  for (const rule of forbidden) {
    rule.pattern.lastIndex = 0;
    const match = rule.pattern.exec(source);
    if (match) failures.push(`${rule.label} in ${path} at byte ${match.index}`);
  }
}

if (failures.length > 0) {
  throw new Error(`Release CSP executable-code check failed:\n${failures.join("\n")}`);
}

console.log(`Release CSP check passed for ${assets.length} JavaScript asset(s).`);
