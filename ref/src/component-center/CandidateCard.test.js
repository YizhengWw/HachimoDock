/**
 * [Input] Read CandidateCard.jsx source.
 * [Output] Static Node coverage: default export, prop signature, explicit game/tool kind
 *          resolution, state-driven card variants without preview status overlays,
 *          two-state device action and dual deletion,
 *          device-screen reuse, complete component copy, and adaptive card geometry CSS.
 * [Pos] test node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "CandidateCard.jsx"), "utf8");
const styles = readFileSync(join(here, "../styles.css"), "utf8");

function extractCssRule(selector) {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const match = styles.match(new RegExp(`${escaped}\\s*\\{([\\s\\S]*?)\\}`));
  assert.ok(match, `missing CSS rule for ${selector}`);
  return match[1];
}

// 1. Default export
test("CandidateCard has a default export function named CandidateCard", () => {
  assert.match(source, /export default function CandidateCard\s*\(/);
});

// 2. Accepts component state plus device and dual-delete callbacks
test("CandidateCard accepts component, kind, local/installed/enabled state, and select/device/delete callbacks", () => {
  assert.match(source, /\{\s*component\b/);
  assert.match(source, /\bkind\b/);
  assert.match(source, /\bisLocal\b/);
  assert.match(source, /\bisInstalled\b/);
  assert.match(source, /\bisEnabled\b/);
  assert.match(source, /\bonClick\b/);
  assert.match(source, /\bonDeviceAction\b/);
  assert.match(source, /\bonDelete\b/);
  assert.doesNotMatch(source, /\bonRemove\b/);
});

// 3. Renders candidate-card root class
test("CandidateCard renders an element with class candidate-card", () => {
  assert.match(source, /candidate-card/);
});

// 4. Badge variant switches on isLocal
test("CandidateCard labels formal local and builtin sources", () => {
  assert.match(source, /const sourceKind = component\.isDeviceOnly \? "device"/);
  assert.match(source, /const sourceLabel = component\.isDeviceOnly \? "仅设备"/);
  assert.match(source, /candidate-card__badge--\$\{sourceKind\}/);
});

test("CandidateCard tags builtin and local cards with source modifier classes", () => {
  assert.match(source, /candidate-card--\$\{component\.isDeviceOnly \? "device" : isLocal \? "local" : "builtin"\}/);
});

test("CandidateCard resolves explicit kind before falling back from gameType", () => {
  const resolverStart = source.indexOf("export function resolveComponentKind");
  const resolverEnd = source.indexOf("export function componentKindLabel", resolverStart);
  const resolver = source.slice(resolverStart, resolverEnd);
  assert.notEqual(resolverStart, -1);
  assert.notEqual(resolverEnd, -1);
  assert.ok(
    resolver.indexOf('["game", "mini-game", "minigame", "小游戏"]') < resolver.indexOf("return gameType"),
  );
  assert.ok(
    resolver.indexOf('["tool", "utility", "widget", "工具", "工具组件"]') < resolver.indexOf("return gameType"),
  );
  assert.match(source, /return kind === "game" \? "小游戏" : "工具组件"/);
  assert.match(source, /resolveComponentKind\(kind, component\.gameType\)/);
});

test("CandidateCard marks the running item semantically without a preview status overlay", () => {
  assert.match(source, /aria-current=\{isEnabled \? "true" : undefined\}/);
  assert.match(source, /candidate-card--enabled/);
  const enabledRule = extractCssRule(".candidate-card--enabled");
  assert.match(enabledRule, /border-color:/);
  assert.doesNotMatch(source, /candidate-card__enabled-badge/);
  assert.doesNotMatch(source, />\s*当前启用\s*</);
});

test("CandidateCard exposes exactly two immediate device states and dual-delete access", () => {
  assert.match(source, /onDeviceAction/);
  assert.match(source, /同步到设备/);
  assert.match(source, /已同步到设备（点击从设备删除）/);
  assert.match(source, /candidate-card__install/);
  assert.match(source, /isInstalled/);
  assert.match(source, /isLocal && onDelete/);
  assert.match(source, /candidate-card__delete/);
  assert.match(source, /aria-label=\{`从电脑和设备删除 \$\{component\.name\}`\}/);
  assert.match(source, /onClick=\{\(event\) => \{[\s\S]*?event\.stopPropagation\(\);[\s\S]*?onDelete\(\);[\s\S]*?\}\}/);
  assert.doesNotMatch(source, /pendingSync|待同步|DeviceComponentOverview/);
});

test("CandidateCard keeps board presence in the bottom action without a preview status overlay", () => {
  assert.match(source, /candidate-card--installed/);
  assert.match(source, /已同步到设备（点击从设备删除）/);
  assert.doesNotMatch(source, /candidate-card__installed-badge/);
  assert.doesNotMatch(source, />\s*已同步\s*</);
});

// 5. Uses DeviceScreenPreview shared helper (which renders component-device-screen internally)
test("CandidateCard imports and uses DeviceScreenPreview for the mini preview", () => {
  assert.match(source, /import DeviceScreenPreview from ['"]\.\/DeviceScreenPreview['"]/);
  assert.match(source, /DeviceScreenPreview/);
  assert.match(source, /candidate-card__screen/);
});

test("CandidateCard CSS keeps builtin, local, and create cards on one adaptive preview rhythm", () => {
  const cardRule = extractCssRule(".candidate-card");
  const previewRule = extractCssRule(".candidate-card__preview");
  const screenRule = extractCssRule(".candidate-card__screen");
  const createRule = extractCssRule(".candidate-card__preview--create");
  assert.match(cardRule, /min-height:/);
  assert.match(previewRule, /height:\s*auto/);
  assert.doesNotMatch(previewRule, /height:\s*clamp\(/);
  assert.match(screenRule, /width:\s*100%/);
  assert.match(screenRule, /max-width:\s*100%/);
  assert.doesNotMatch(screenRule, /320px/);
  assert.doesNotMatch(createRule, /height:\s*80px/);
});

test("CandidateCard CSS leaves enough room for complete preview and copy", () => {
  const previewRule = extractCssRule(".candidate-card__preview");
  const goalRule = extractCssRule(".candidate-card__goal");
  assert.match(previewRule, /box-sizing:\s*border-box/);
  assert.doesNotMatch(previewRule, /min-height:\s*248px/);
  assert.doesNotMatch(goalRule, /-webkit-line-clamp/);
  assert.doesNotMatch(goalRule, /overflow:\s*hidden/);
});

test("CandidateCard preview frame is the sizing source and clips oversized screen content", () => {
  const cardRule = extractCssRule(".candidate-card");
  const previewRule = extractCssRule(".candidate-card__preview");
  const screenRule = extractCssRule(".candidate-card__screen");
  assert.match(cardRule, /align-items:\s*stretch/);
  assert.match(previewRule, /width:\s*100%/);
  assert.match(previewRule, /align-self:\s*stretch/);
  assert.match(previewRule, /overflow:\s*hidden/);
  assert.match(screenRule, /display:\s*block/);
});
