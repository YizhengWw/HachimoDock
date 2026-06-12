/**
 * [Input] Read ChannelMatrixCard.jsx source.
 * [Output] Static Node coverage: default export, detected-agent filtering, useDeviceContext field destructuring, followed row state, per-agent appearance saving, followed-agent device sync, AgentAppearancePickerModal subcomponent and compact picker modal CSS.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "ChannelMatrixCard.jsx"), "utf8");
const styles = readFileSync(join(here, "../styles.css"), "utf8");

function extractCssRule(css, selector) {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const match = css.match(new RegExp(`${escaped}\\s*\\{([\\s\\S]*?)\\n\\}`));
  return match ? match[1] : "";
}

test("ChannelMatrixCard has a default export", () => {
  assert.match(source, /export default function ChannelMatrixCard\s*\(/);
});

test("ChannelMatrixCard renders detected local agents only", () => {
  assert.match(source, /installedAgents/);
  assert.match(source, /agentOptions\.filter\(\(agent\) => agent\.detected\)/);
  assert.match(source, /installedAgents\.map\s*\(/);
});

test("ChannelMatrixCard destructures multiple fields from useDeviceContext", () => {
  assert.match(source, /useDeviceContext\s*\(\s*\)/);
  assert.match(source, /appearances/);
  assert.match(source, /agentAppearanceMap/);
  assert.match(source, /agentOptions/);
  assert.match(source, /currentDisplay/);
  assert.match(source, /deviceConnected/);
  assert.match(source, /applyDesktopPet/);
  assert.match(source, /saveAgentAppearance/);
});

test("ChannelMatrixCard applies is-active class to the active channel row", () => {
  assert.match(source, /is-active/);
  assert.match(source, /isFollowed/);
  // Must be conditional based on active channel
  assert.match(source, /isFollowed\s*\?\s*["'].*is-active.*["']/);
});

test("Non-followed agents show a follow button while the active one is marked followed", () => {
  assert.match(source, /已跟随/);
  assert.match(source, /跟随/);
  assert.match(source, /channel-row__follow-current/);
  assert.match(source, /channel-row__follow-button/);
  assert.match(source, /requestFollow\(agent\.id\)/);
});

test("Agent appearance changes save locally unless the agent is currently followed", () => {
  assert.match(source, /BUILTIN_TERRIER_APPEARANCE_ID/);
  assert.match(source, /saveAgentAppearance\(agentId, appearance\.id\)/);
  assert.match(source, /agentId === activeAgentId/);
  assert.match(source, /applyDesktopPet\(agentId, appearance/);
});

test("Follow confirmation syncs the selected agent appearance to the device", () => {
  assert.match(source, /setPendingFollow\(\{ agentId, appearance \}\)/);
  assert.match(source, /function FollowAgentConfirmModal\s*\(/);
  assert.match(source, /确认跟随/);
  assert.match(source, /同步「\{appearanceName\}」到设备端展示/);
});

test("AgentAppearancePickerModal subcomponent is declared in the same file", () => {
  assert.match(source, /function AgentAppearancePickerModal\s*\(/);
});

test("FormosaPickerModal CSS stays compact when only a few appearances exist", () => {
  const modalRule = extractCssRule(styles, ".modal-card--formosa-picker");
  const gridRule = extractCssRule(styles, ".formosa-picker__grid");
  const pickerStageRule = extractCssRule(styles, ".formosa-picker__stage");
  const pickerMediaRule = extractCssRule(styles, ".formosa-picker__media");
  assert.match(modalRule, /width:\s*min\(/);
  assert.doesNotMatch(modalRule, /(^|\n)\s*width:\s*100%/);
  assert.match(gridRule, /repeat\(auto-fit,\s*minmax\(140px,\s*180px\)\)/);
  assert.doesNotMatch(gridRule, /repeat\(auto-fill,/);
  assert.match(gridRule, /justify-content:\s*start/);
  assert.match(source, /className="formosa-picker__stage"/);
  assert.match(pickerStageRule, /height:\s*150px/);
  assert.match(pickerStageRule, /place-items:\s*center/);
  assert.match(pickerStageRule, /background:\s*#000/);
  assert.match(pickerMediaRule, /object-fit:\s*contain/);
  assert.match(pickerMediaRule, /object-position:\s*center\s+center/);
  assert.match(styles, /\.channel-row__formosa\s*\{[\s\S]*grid-template-columns:\s*96px\s+minmax\(0,\s*1fr\)/);
  assert.match(styles, /\.channel-row__thumb-wrap\s*\{[\s\S]*background:\s*#000/);
  assert.match(styles, /\.channel-row__thumb:is\(img,\s*video\)\s*\{[\s\S]*object-fit:\s*contain/);
  assert.match(styles, /\.modal-card--formosa-picker\s+\.modal-footer\s*\{/);
});
