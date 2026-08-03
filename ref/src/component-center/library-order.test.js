/**
 * [Input] library-order.js component creation-time helpers.
 * [Output] Node regression coverage for mixed builtin/custom newest-first ordering and stable ties.
 * [Pos] test node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  componentCreatedAtMs,
  sortComponentsByCreatedAt,
} from "./library-order.js";

test("component library sorts mixed records by creation time newest first", () => {
  const sorted = sortComponentsByCreatedAt([
    { id: "builtin-old", createdAt: "2026-05-24T15:17:05+08:00" },
    { id: "draft-new", createdAtMs: Date.parse("2026-07-28T10:00:00+08:00") },
    { id: "builtin-new", createdAt: "2026-07-23T19:19:17+08:00" },
  ]);

  assert.deepEqual(sorted.map((item) => item.id), [
    "draft-new",
    "builtin-new",
    "builtin-old",
  ]);
});

test("component library keeps source order for equal or missing creation times", () => {
  const items = [
    { id: "first", createdAtMs: 100 },
    { id: "second", createdAtMs: 100 },
    { id: "missing-a" },
    { id: "missing-b" },
  ];

  assert.deepEqual(
    sortComponentsByCreatedAt(items).map((item) => item.id),
    items.map((item) => item.id),
  );
  assert.equal(componentCreatedAtMs({ createdAt: "not-a-date" }), 0);
});
