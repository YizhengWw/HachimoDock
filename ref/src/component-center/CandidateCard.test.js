/**
 * [Input] Read CandidateCard.jsx source.
 * [Output] Static Node coverage: default export, prop signature, class names, badge/source
 *          variants, device-screen reuse, draft delete action, complete component copy, and adaptive
 *          card geometry CSS.
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

// 2. Accepts component, isDraft, onClick, onDelete props
test("CandidateCard accepts component, isDraft, onClick, and onDelete props", () => {
  assert.match(source, /\{\s*component\b/);
  assert.match(source, /\bisDraft\b/);
  assert.match(source, /\bonClick\b/);
  assert.match(source, /\bonDelete\b/);
});

// 3. Renders candidate-card root class
test("CandidateCard renders an element with class candidate-card", () => {
  assert.match(source, /candidate-card/);
});

// 4. Badge variant switches on isDraft
test("CandidateCard applies --custom badge when isDraft and --builtin badge otherwise", () => {
  assert.match(source, /candidate-card__badge--\$\{isDraft \? "custom" : "builtin"\}/);
  assert.match(source, /isDraft \? "自定义" : "内置"/);
});

test("CandidateCard tags builtin and draft cards with source modifier classes", () => {
  assert.match(source, /candidate-card--\$\{isDraft \? "draft" : "builtin"\}/);
});

test("CandidateCard renders an independent delete button only for draft components", () => {
  assert.match(source, /isDraft && onDelete/);
  assert.match(source, /candidate-card__delete/);
  assert.match(source, /aria-label=\{`删除 \$\{component\.name\}`\}/);
  assert.match(source, /onClick=\{\(event\) => \{[\s\S]*?event\.stopPropagation\(\);[\s\S]*?onDelete\(\);[\s\S]*?\}\}/);
});

// 5. Uses DeviceScreenPreview shared helper (which renders component-device-screen internally)
test("CandidateCard imports and uses DeviceScreenPreview for the mini preview", () => {
  assert.match(source, /import DeviceScreenPreview from ['"]\.\/DeviceScreenPreview['"]/);
  assert.match(source, /DeviceScreenPreview/);
  assert.match(source, /candidate-card__screen/);
});

test("CandidateCard CSS keeps builtin, draft, and create cards on one adaptive preview rhythm", () => {
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
