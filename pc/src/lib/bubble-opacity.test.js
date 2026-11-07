import test from "node:test";
import assert from "node:assert/strict";
import { normalizeBubbleOpacity } from "./bubble-opacity.js";

test("bubble opacity stays readable and bounded", () => {
  assert.equal(normalizeBubbleOpacity(0), 20);
  assert.equal(normalizeBubbleOpacity(200), 100);
  assert.equal(normalizeBubbleOpacity("80"), 80);
});
