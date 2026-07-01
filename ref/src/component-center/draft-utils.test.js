/**
 * [Input] The draft helpers exported by `./draft-utils.js`.
 * [Output] Behavior coverage (bare node) for draft summary copy and the
 *          cross-separator clawpkg-path matching ComponentCenter relies on.
 * [Pos] test node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  buildDraftGoal,
  matchesDraftPath,
  normalizeLocalPath,
  pathContainsComponentId,
} from "./draft-utils.js";

test("buildDraftGoal prefers the trimmed description, else a default summary", () => {
  assert.equal(buildDraftGoal({ description: "  我的组件  " }), "我的组件");
  assert.equal(buildDraftGoal({ description: "   " }), "自定义草稿 · 可预览后安装到负一屏。");
  assert.equal(buildDraftGoal({}), "自定义草稿 · 可预览后安装到负一屏。");
});

test("normalizeLocalPath collapses Windows separators to forward slashes", () => {
  assert.equal(normalizeLocalPath("C:\\drafts\\token-meter"), "C:/drafts/token-meter");
  assert.equal(normalizeLocalPath(null), "");
});

test("pathContainsComponentId matches an id segment, .clawpkg, or .zip", () => {
  assert.equal(pathContainsComponentId("/a/token-meter/widget.json", "token-meter"), true);
  assert.equal(pathContainsComponentId("C:\\pkgs\\token-meter.clawpkg", "token-meter"), true);
  assert.equal(pathContainsComponentId("/a/token-meter.zip", "token-meter"), true);
  assert.equal(pathContainsComponentId("/a/other/widget.json", "token-meter"), false);
  assert.equal(pathContainsComponentId("/a/b", ""), false);
});

test("matchesDraftPath matches on equal normalized paths or an id-bearing path", () => {
  assert.equal(matchesDraftPath({ path: "C:\\d\\x", id: "x" }, "C:/d/x"), true);
  assert.equal(matchesDraftPath({ path: "/elsewhere", id: "token-meter" }, "/pkgs/token-meter.clawpkg"), true);
  assert.equal(matchesDraftPath({ path: "/a", id: "x" }, "/b/y.clawpkg"), false);
  assert.equal(matchesDraftPath(null, "/a"), false);
});
