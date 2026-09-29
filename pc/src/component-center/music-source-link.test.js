import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { MUSIC_SOURCE_URL, openMusicSourceLink } from "./music-source-link.js";

test("desktop source link uses the native external browser command", async () => {
  const calls = [], errors = [];
  let prevented = false;
  await openMusicSourceLink({ preventDefault() { prevented = true; } }, {
    isDesktop: true,
    invokeExternal: async (...args) => calls.push(args),
    onError: error => errors.push(error),
  });
  assert.equal(prevented, true);
  assert.deepEqual(calls, [["open_external_url", { url: MUSIC_SOURCE_URL }]]);
  assert.deepEqual(errors, [""]);
});

test("web source link keeps browser navigation without a native call", async () => {
  await openMusicSourceLink({ preventDefault() { assert.fail("must retain default link navigation"); } }, {
    isDesktop: false,
    invokeExternal: () => assert.fail("must not invoke native APIs"),
    onError: () => assert.fail("must not change error state"),
  });
});

test("failed native open gives a visible copyable URL instead of silently failing", async () => {
  const errors = [];
  await openMusicSourceLink({ preventDefault() {} }, {
    isDesktop: true,
    invokeExternal: async () => { throw new Error("unavailable"); },
    onError: error => errors.push(error),
  });
  assert.equal(errors.length, 2);
  assert.ok(errors[1].includes(MUSIC_SOURCE_URL));
  assert.match(errors[1], /无法打开浏览器/);
});

test("music attribution appears once, directly below the top heading", () => {
  const source = readFileSync(new URL("./MusicPlayer.jsx", import.meta.url), "utf8");
  assert.match(source, /<h3>让喜欢的音乐，留在桌边<\/h3>\s*<p className="music-library__credit">感谢/);
  assert.equal((source.match(/className="music-library__credit"/g) || []).length, 1);
  assert.match(source, /href=\{MUSIC_SOURCE_URL\}[^>]+onClick=\{event => openMusicSourceLink/);
  assert.match(source, /sourceLinkError && <p[^>]+role="alert"/);
  assert.match(source, /提供 API 服务/);
  assert.match(source, /回到宠物主页后，再按一次退出键即可停止音乐/);
  assert.doesNotMatch(source, /提供在线音乐服务/);
});

test("music search uses automatic source selection without exposing a provider selector", () => {
  const source = readFileSync(new URL("./MusicPlayer.jsx", import.meta.url), "utf8");
  assert.doesNotMatch(source, /aria-label="音乐源"|setSource\(/);
  assert.match(source, /input: \{ operation: "search", query: query.trim\(\) \}/);
  assert.match(source, /aria-label="搜索歌曲"/);
});
