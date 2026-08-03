/*
 * [Input] board-runtime/src/board_server.c remote binding handler.
 * [Output] Regression coverage that remote binding updates republish availability with targetSource.
 */

const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { resolve } = require("node:path");
const test = require("node:test");

const source = readFileSync(resolve(__dirname, "../src/board_server.c"), "utf8");

test("remote binding re-publishes availability after targetSource changes", () => {
  const match = source.match(/static void br_server_rebind[\s\S]*?\n}/);
  assert.ok(match, "expected br_server_rebind function");
  assert.match(match[0], /br_server_publish_presence\(\s*server,\s*true\s*\)/);
});
