import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { parseSync } from "@babel/core";
import traverseModule from "@babel/traverse";

const source = readFileSync(new URL("../ComponentCenter.jsx", import.meta.url), "utf8");
const ast = parseSync(source, { sourceType: "module", parserOpts: { plugins: ["jsx"] } });
const declaration = ast.program.body.find(node => node.type === "FunctionDeclaration" && node.id.name === "gameInstallBlockedReason");
const check = vm.runInNewContext(`(${source.slice(declaration.start, declaration.end)})`);
const usb = { capabilities: { widgetLyrics: "p4-lrc-v1", widgetMedia: "p4-media-v1", widgetData: "p4-data-list-v1", widgetRuntime: "p4-bounded-runtime-v4", widgetScene: "p4-scene-v1" } };

test("opening ordinary, data and media components executes the actual shared capability check", () => {
  for (const component of [
    { id: "music-player", mediaSource: "audio.player", defaultBindings: [{ action: "media.lyrics" }] },
    { id: "stock-watchlist", dataSource: "stocks.watchlist" },
    { id: "todos", dataSource: "todos.upcoming" },
    { id: "computer", dataSource: "computer.status" },
    { id: "game", runtimeEngine: "p4-bounded-runtime-v4", sceneEngine: "p4-scene-v1" },
    { id: "plain-tool", defaultBindings: [] },
  ]) assert.equal(check(component, usb), "", component.id);
});

test("missing/older device capabilities give a reason rather than a render exception", () => {
  assert.match(check({ defaultBindings: [{ action: "media.lyrics" }] }, {}), /歌词模式/);
  assert.match(check({ id: "music-player" }, {}), /音乐播放/);
  assert.match(check({ dataSource: "stocks.watchlist" }, undefined), /实时数据/);
  assert.match(check({ runtimeEngine: "p4-bounded-runtime-v4" }, {}), /通用运行时/);
  assert.equal(check({}, undefined), "");
  assert.equal(check({ defaultBindings: [null] }, {}), "");
  assert.equal(check({ defaultBindings: {} }, {}), "");
});

test("lyric capability follows actions, not which physical key the user mapped", () => {
  for (const key of ["sw1", "sw2"]) {
    const event = `button.${key}.long_press`;
    const component = { defaultBindings: [{ action: "media.lyrics", event }] };
    assert.match(check(component, {}), /歌词模式/);
    assert.equal(check(component, usb), "");
  }
});

test("ComponentCenter has no unresolved references across function scopes", () => {
  const traverse = traverseModule.default || traverseModule;
  const globals = new Set([...Object.getOwnPropertyNames(globalThis), "window", "document", "navigator", "localStorage"]);
  const unknown = new Set();
  traverse(ast, { ReferencedIdentifier(path) {
    if (!path.scope.hasBinding(path.node.name) && !globals.has(path.node.name)) unknown.add(path.node.name);
  } });
  assert.deepEqual([...unknown], []);
});

test("the app isolates ComponentCenter itself, including modal prop evaluation", () => {
  const app = readFileSync(new URL("../App.jsx", import.meta.url), "utf8");
  assert.match(app, /<ComponentCenterBoundary><ComponentCenter\s*\/><\/ComponentCenterBoundary>/);
});
