/**
 * [Input] Read NowShowingHero.jsx source.
 * [Output] Static Node coverage that NowShowingHero renders the bound hero (preview + bindings + 更换 button)
 *          and the passive empty state when component is null.
 * [Pos] test node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "NowShowingHero.jsx"), "utf8");

// 1. Default export
test("NowShowingHero exports a default React component", () => {
  assert.match(source, /export default function NowShowingHero\s*\(/);
});

// 2. Imports Card from shell
test("NowShowingHero imports Card from shell", () => {
  assert.match(source, /import Card from ['"]\.\.\/shell\/Card['"]/);
});

// 3. Bound state class names
test("NowShowingHero renders now-showing-hero, __preview, __body when component is truthy", () => {
  assert.match(source, /className="now-showing-hero"/);
  assert.match(source, /now-showing-hero__preview/);
  assert.match(source, /now-showing-hero__body/);
});

// 4. Empty state branch
test("NowShowingHero renders now-showing-hero--empty when component is null", () => {
  assert.match(source, /now-showing-hero--empty/);
});

// 5. Uses DeviceScreenPreview shared helper (which renders component-device-screen internally)
test("NowShowingHero imports and uses DeviceScreenPreview for the device preview", () => {
  assert.match(source, /import DeviceScreenPreview from ['"]\.\/DeviceScreenPreview['"]/);
  assert.match(source, /DeviceScreenPreview/);
});

// 6. Button bindings section with heading and map
test("NowShowingHero renders 按钮映射 heading and iterates buttonBindings.map", () => {
  assert.match(source, /按钮映射/);
  assert.match(source, /buttonBindings\.map/);
});

test("NowShowingHero renders human-readable control gesture labels", () => {
  assert.match(source, /formatBindingControl/);
  assert.match(source, /now-showing-hero__binding-control/);
});

// 7. 更换组件 button with disabled guard
test("NowShowingHero renders 更换组件 button disabled when deviceConnected is false", () => {
  assert.match(source, /更换组件/);
  assert.match(source, /disabled=\{!deviceConnected\}/);
});

// 8. Empty state has icon but no redundant browse-library CTA
test("NowShowingHero empty state has no 浏览组件库 button", () => {
  const emptyStart = source.indexOf("now-showing-hero now-showing-hero--empty");
  const emptyEnd = source.indexOf("</Card>", emptyStart);
  const emptyBlock = source.slice(emptyStart, emptyEnd);
  assert.notEqual(emptyStart, -1);
  assert.notEqual(emptyEnd, -1);
  assert.match(source, /📦/);
  assert.doesNotMatch(source, /浏览组件库/);
  assert.doesNotMatch(emptyBlock, /<button/);
});
