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
test("automatic media never silently truncates on old firmware",()=>{
 const c={pcExtension:{discovery:{autoPlayOnEnter:true}}};
 assert.match(check(c,usb),/自动电台/);
 assert.equal(check(c,{capabilities:{widgetMedia:"p4-media-v1",widgetMediaSurface:"p4-media-surface-v1",widgetMediaQueueMax:128,widgetMediaLifecycle:"p4-media-lifecycle-v1"}}),"");
});
test("lifecycle, media touch and wide atlases cannot silently downgrade",()=>{
  for(const [component,key,value] of [
    [{inputPolicy:{engine:"p4-input-v1"}},"widgetInput","p4-input-v1"],
    [{appRuntime:{input:{engine:"p4-input-v1"}}},"widgetInput","p4-input-v1"],
    [{appRuntime:{surface:{fonts:[{cell:[32,48]}]}}},"widgetGlyphCells","p4-glyph-cells-v1"],
    [{appRuntime:{lifecycle:{pause_action:"pause"}}},"widgetLifecycle","p4-lifecycle-v1"],
    [{appRuntime:{surface:{input:"p4-media-touch-v1"}}},"widgetMediaTouch","p4-media-touch-v1"],
    [{appRuntime:{surface:{images:[{frame_width:480}]}}},"widgetSurfaceAtlas","p4-wide-atlas-v1"],
  ]){assert.notEqual(check(component,usb),"");assert.equal(check(component,{capabilities:{[key]:value}}),"");}
});
test("package-owned flight speed cannot silently fall back to fast old firmware",()=>{
  const c={appRuntime:{game:{type:"flappy",vertical_speed_percent:50}}};
  assert.match(check(c,usb),/飞鸟速度/);
  assert.equal(check(c,{capabilities:{widgetFlappyTuning:"p4-flappy-tuning-v1"}}),"");
});
test("classic game surfaces reject old firmware before any transfer",()=>{
  const c={appRuntime:{surface:{game_view:{engine:"p4-game-surface-v1"}}}};
  assert.match(check(c,usb),/精细游戏画面/);
  assert.equal(check(c,{capabilities:{widgetGameSurface:"p4-game-surface-v1"}}),"");
});

test("touch revision 2 cannot silently install onto the earlier single-mode firmware",()=>{
  const component={scene:{engine:"p4-touch-slice-v1",revision:2},sceneEngine:"p4-touch-slice-v1",runtimeEngine:"p4-bounded-runtime-v4"};
  const device={capabilities:{widgetPointer:"p4-pointer-v1",widgetRuntime:"p4-bounded-runtime-v4",widgetScenes:["p4-touch-slice-v1"],touchInput:{ready:true}}};
  assert.match(check(component,device),/新版切果/);
  device.capabilities.widgetSliceRevision=2;
  assert.equal(check(component,device),"");
  device.capabilities.touchInput.ready=false;
  assert.match(check(component,device),/连续触控/);
});

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
  assert.match(app, /<ComponentCenterBoundary><ComponentCenter active=\{view === "components"\}\s*\/><\/ComponentCenterBoundary>/);
});

test("image surfaces require explicit firmware support",()=>{
 const c={mediaSurface:{images:[{id:"art"}]}};
 assert.match(check(c,{}),/图片与旋转/);
 assert.equal(check(c,{capabilities:{widgetSurfaceImages:"p4-surface-images-v1"}}),"");
});

test("app graphics cannot be silently installed on legacy app-surface firmware",()=>{
 const c={mediaSurface:{engine:"p4-app-surface-v1",graphics:"p4-app-graphics-v1",images:[{id:"background"}]}};
 const capabilities={widgetAppSurface:"p4-app-surface-v1",widgetSurfaceImages:"p4-surface-images-v1"};
 assert.match(check(c,{capabilities}),/高清背景/);
 assert.equal(check(c,{capabilities:{...capabilities,widgetAppGraphics:"p4-app-graphics-v1"}}),"");
});
