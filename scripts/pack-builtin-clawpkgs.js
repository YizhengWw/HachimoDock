#!/usr/bin/env node
/**
 * Mirror ref/builtin-clawpkgs/<id>/ to ~/.openclaw/builtin-clawpkgs/<id>/
 * and pack ~/.openclaw/builtin-clawpkgs/<id>.clawpkg via the system `zip`
 * command. Run on first install or manually.
 */
const { cpSync, readdirSync, statSync, existsSync, mkdirSync, rmSync } = require("node:fs");
const { execFileSync } = require("node:child_process");
const { resolve, dirname, join } = require("node:path");
const { homedir } = require("node:os");

const here = __dirname;
const srcRoot = resolve(here, "../ref/builtin-clawpkgs");
const outRoot = resolve(homedir(), ".openclaw/builtin-clawpkgs");

if (!existsSync(srcRoot)) {
  console.error(`source directory missing: ${srcRoot}`);
  process.exit(1);
}
mkdirSync(outRoot, { recursive: true });

const built = [];
for (const id of readdirSync(srcRoot)) {
  const dir = join(srcRoot, id);
  if (!statSync(dir).isDirectory()) continue;
  const outDir = join(outRoot, id);
  const out = join(outRoot, `${id}.clawpkg`);
  if (existsSync(outDir)) rmSync(outDir, { recursive: true, force: true });
  if (existsSync(out)) rmSync(out);
  cpSync(dir, outDir, { recursive: true });
  try {
    execFileSync("zip", ["-rq", out, "."], { cwd: dir });
    console.log(`mirrored ${id} -> ${outDir}`);
    console.log(`packed ${id} -> ${out}`);
    built.push(out);
  } catch (err) {
    console.error(`pack failed for ${id}: ${err.message}`);
    process.exit(1);
  }
}
console.log(`\n${built.length} clawpkg(s) packed.`);
