/**
 * [Input] Optional test root relative to the caller's current directory.
 * [Output] Recursively discovers *.test.js files and runs them with Node's test runner.
 * [Pos] Cross-platform repository test launcher used by pc/package.json.
 * [Sync] If this file changes, update scripts/.folder.md.
 */

import { readdir } from "node:fs/promises";
import { resolve } from "node:path";
import { spawnSync } from "node:child_process";

async function collectTests(directory) {
  const entries = await readdir(directory, { withFileTypes: true });
  const tests = [];

  for (const entry of entries) {
    const path = resolve(directory, entry.name);
    if (entry.isDirectory()) {
      tests.push(...await collectTests(path));
    } else if (entry.isFile() && entry.name.endsWith(".test.js")) {
      tests.push(path);
    }
  }

  return tests;
}

const roots = (process.argv.slice(2).length > 0 ? process.argv.slice(2) : ["src"])
  .map((root) => resolve(process.cwd(), root));
const tests = (await Promise.all(roots.map((root) => collectTests(root))))
  .flat()
  .sort();

if (tests.length === 0) {
  console.error(`No *.test.js files found under ${roots.join(", ")}`);
  process.exit(1);
}

const result = spawnSync(process.execPath, ["--test", ...tests], { stdio: "inherit" });
if (result.error) {
  console.error(result.error.message);
}
process.exit(result.status ?? 1);
