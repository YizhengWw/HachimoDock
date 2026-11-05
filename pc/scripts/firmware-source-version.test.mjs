import test from "node:test";
import assert from "node:assert/strict";
import { p4FirmwareSourceVersion } from "./firmware-source-version.mjs";

test("P4 packaging ignores an earlier StopWatch project version", () => {
  const source = [
    'if(HACHIMO_BOARD STREQUAL "m5stack_stopwatch_v1")',
    '  set(PROJECT_VER "0.1.1-stopwatch-dev")',
    'endif()',
    'set(PROJECT_VER "0.7.103-p4-r179")',
  ].join("\n");
  assert.equal(p4FirmwareSourceVersion(source), "0.7.103-p4-r179");
});
test("legacy single-target CMake and variable overrides retain the P4 version", () => {
  assert.equal(p4FirmwareSourceVersion(
    'set(PROJECT_VER "0.7.103-p4-r2")\nset(PROJECT_VER "$ENV{PET_P4_PROJECT_VER_OVERRIDE}")'
  ), "0.7.103-p4-r2");
});
test("commented and missing P4 versions are not accepted", () => {
  assert.throws(() => p4FirmwareSourceVersion(
    '# set(PROJECT_VER "0.7.103-p4-r2")\nset(PROJECT_VER "0.1.1-stopwatch-dev")'
  ), /唯一/);
});
test("ambiguous P4 versions fail before packaging", () => {
  assert.throws(() => p4FirmwareSourceVersion(
    'set(PROJECT_VER "0.7.103-p4-r2")\nset(PROJECT_VER "0.7.103-p4-r179")'
  ), /唯一/);
});
