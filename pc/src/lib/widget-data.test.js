import test from "node:test";
import assert from "node:assert/strict";
import { validateWidgetData } from "./clawpkg-contract.js";
const valid = () => ({ engine: "p4-bounded-runtime-v4", vars: { page: { type: "int", init: 0 } }, data: { source: "stocks.watchlist", page_var: "page" } });
test("bounded live data requires a named source and integer pagination variable", () => {
  const errors = []; validateWidgetData(valid(), errors); assert.deepEqual(errors, []);
  for (const data of [null, [], {}, { source: "https://example.com", page_var: "page" }, { source: "stocks.watchlist", page_var: "missing" }, { source: "stocks.watchlist", page_var: "page", url: "https://example.com" }]) {
    const errors = []; validateWidgetData({ ...valid(), data }, errors); assert.ok(errors.length);
  }
  for (const field of ["scene", "game"]) {
    for (const value of [{}, null]) {
      const errors = []; validateWidgetData({ ...valid(), [field]: value }, errors); assert.ok(errors.length);
    }
  }
  const errors2 = []; validateWidgetData({ vars: {} }, errors2); assert.deepEqual(errors2, []);
});
