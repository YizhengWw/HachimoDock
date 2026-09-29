/**
 * [Input] library-order.js component creation-time helpers.
 * [Output] Node regression coverage for mixed builtin/custom newest-first ordering and stable ties.
 * [Pos] test node in pc/src/component-center
 * [Sync] If this file changes, update `pc/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  componentCreatedAtMs,
  sortComponentsByCreatedAt,
  mergeComponentCatalog,
  isComponentVisible,
} from "./library-order.js";
import { readFileSync } from "node:fs";

test("release hides builtin and already-installed music without deleting records", () => {
  const source = [{id:"stock-watchlist"}, {id:"music-player"}, {id:"wooden-fish"}];
  assert.deepEqual(mergeComponentCatalog([], source).map(x => x.id), ["stock-watchlist", "wooden-fish"]);
  assert.deepEqual(mergeComponentCatalog([{id:"music-player"}], source).map(x => x.id), ["stock-watchlist", "wooden-fish"]);
  assert.equal(source.length, 3);
  assert.equal(isComponentVisible({id:"custom-player", mediaSource:"audio.player"}), false);
  assert.equal(isComponentVisible({id:"upcoming-todos"}), true);
  const center=readFileSync(new URL("../ComponentCenter.jsx", import.meta.url),"utf8");
  assert.ok(center.includes("isComponentVisible(activeComponentRecord)"));
  assert.ok(center.includes("isComponentVisible(item) && !catalogIds.has(item.id)"));
});

test("default builtin tools put wooden fish second on PC and firmware", () => {
  const factory = JSON.parse(readFileSync(new URL("../../../firmware/factory-config.json", import.meta.url)));
  assert.deepEqual(factory.components.ids.slice(0, 5), ["stock-watchlist", "wooden-fish", "music-player", "upcoming-todos", "computer-status"]);
  const fixtures = readFileSync(new URL("../fixtures.js", import.meta.url), "utf8");
  const catalog = fixtures.slice(fixtures.indexOf("export const BUILTIN_COMPONENT_CENTER"));
  assert.ok(catalog.indexOf('id: "stock-watchlist"') < catalog.indexOf("id: woodenFishManifest.id"));
  assert.ok(catalog.indexOf("id: woodenFishManifest.id") < catalog.indexOf("id: musicManifest.id"));
  assert.ok(catalog.indexOf("id: musicManifest.id") < catalog.indexOf("...localDataComponents[1]"));
  assert.ok(catalog.indexOf("...localDataComponents[1]") < catalog.indexOf("...localDataComponents[0]"));
});

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

test("stocks lead default builtins but are not pinned above newly published components", () => {
  const stock = { id: "stock-watchlist", version: "1.1.0" };
  const builtins = [stock, { id: "pong" }, { id: "frog" }];
  const published = [{ id: "custom-new" }, { id: "stock-watchlist", version: "1.0.0" }, { id: "frog", version: "custom" }];
  const result = mergeComponentCatalog(published, builtins);
  assert.deepEqual(result.map((item) => item.id), ["custom-new", "stock-watchlist", "frog", "pong"]);
  assert.equal(result[1], published[1]);
  assert.equal(result[2].version, "custom");
  assert.deepEqual(mergeComponentCatalog([], builtins), builtins);
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
