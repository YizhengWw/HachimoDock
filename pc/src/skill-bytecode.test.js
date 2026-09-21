import test from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, writeFileSync, existsSync, symlinkSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { cleanSkillBytecode } from "../scripts/clean-skill-bytecode.mjs";
test("packaging removes bytecode without following symlinks or deleting source", () => {
  const root = mkdtempSync(join(tmpdir(), "pet-skill-bytecode-test-"));
  try {
    const skill = join(root, "skill"), cache = join(skill, "__pycache__"), outside = join(root, "external");
    mkdirSync(cache, { recursive: true }); mkdirSync(outside);
    writeFileSync(join(cache, "validate.pyc"), "generated");
    writeFileSync(join(skill, "validate.py"), "source");
    writeFileSync(join(outside, "keep.pyc"), "external");
    symlinkSync(outside, join(skill, "linked"));
    assert.equal(cleanSkillBytecode(skill), 1);
    assert.equal(existsSync(cache), false);
    assert.equal(existsSync(join(skill, "validate.py")), true);
    assert.equal(existsSync(join(outside, "keep.pyc")), true);
    assert.equal(cleanSkillBytecode(skill), 0);
  } finally { rmSync(root, { recursive: true, force: true }); }
});
