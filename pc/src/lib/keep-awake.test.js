import test from "node:test";
import assert from "node:assert/strict";
import { normalizeKeepAwake } from "./keep-awake.js";

test("keep-awake defaults off and accepts only explicit true", () => {
  assert.equal(normalizeKeepAwake(false), false);
  assert.equal(normalizeKeepAwake(null), false);
  assert.equal(normalizeKeepAwake("false"), false);
  assert.equal(normalizeKeepAwake(true), true);
  assert.equal(normalizeKeepAwake("true"), true);
});
