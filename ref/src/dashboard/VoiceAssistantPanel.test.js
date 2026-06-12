/**
 * [Input] Read VoiceAssistantPanel.jsx source.
 * [Output] Static Node coverage that the voice panel no longer embeds button config, exposes the voice controls plus mock/session injection status through shared form controls, is suitable for placement inside Card.Collapsible, and provides a summary helper.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "VoiceAssistantPanel.jsx"), "utf8");

test("VoiceAssistantPanel exports a default React component", () => {
  assert.match(source, /export default function VoiceAssistantPanel\s*\(/);
});

test("Button config is no longer embedded (moved to BoardButtonPanel)", () => {
  assert.doesNotMatch(source, /BoardButtonConfigPanel/);
  assert.doesNotMatch(source, /BoardButtonPanel/);
  assert.doesNotMatch(source, /voice-button-action-list/);
});

test("Exposes the 3 voice controls — switch + session select + listen button", () => {
  assert.match(source, /voice-config-switch/);
  assert.match(source, /是否开启语音/);
  assert.match(source, /voice-session-select/);
  assert.match(source, /启动板端麦克风收听|停止板端收听/);
});

test("Shows mock text injection and board voice action result status", () => {
  assert.match(source, /sendMockButtonInject/);
  assert.match(source, /测试输入（模拟按钮语音转文字）/);
  assert.match(source, /发送到当前会话/);
  assert.match(source, /模型回复预览/);
  assert.match(source, /deviceVoiceFlow/);
  assert.match(source, /设备语音状态/);
  assert.match(source, /发送中/);
  assert.match(source, /等待回复/);
  assert.match(source, /设备识别文本/);
  assert.match(source, /设备语音回复预览/);
});

test("Uses shared form controls instead of one-off native-looking fields", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");

  assert.match(source, /ui-field/);
  assert.match(source, /ui-control-shell/);
  assert.match(source, /ui-control ui-control--select/);
  assert.match(source, /ui-control ui-control--textarea/);
  assert.match(source, /ui-control-shell__chevron/);
  assert.doesNotMatch(source, /voice-panel__session-select/);
  assert.doesNotMatch(source, /voice-panel__mock-input/);

  assert.match(css, /\.ui-control\s*\{/);
  assert.match(css, /\.ui-control-shell\s*\{/);
  assert.match(css, /\.ui-control--select\s*\{/);
  assert.match(css, /\.ui-control--select option\s*\{/);
  assert.match(css, /\.ui-control--select option:checked\s*\{/);
  assert.match(css, /\.ui-control--textarea\s*\{/);
  assert.doesNotMatch(css, /\.voice-panel__mock\s*\{[^}]*background:/);
});

test("Exports a helper for the Card.Collapsible summary string", () => {
  assert.match(source, /export function buildVoiceSummary\s*\(/);
});

test("Session option labels prefer the Codex client thread name", () => {
  assert.match(source, /const name = typeof session\.name === "string"/);
  assert.match(source, /if \(name\) parts\.push\(name\)/);
  assert.match(source, /if \(cwdName && !name\)/);
  assert.match(source, /if \(!name && summary\)/);
});

test("buildVoiceSummary returns 已开启/未开启 plus the trigger label", () => {
  // Source check — runtime test added in Task 7 integration.
  assert.match(source, /已开启/);
  assert.match(source, /未开启/);
});

test("Does not render its own card chrome — leaves card wrapping to the parent", () => {
  assert.doesNotMatch(source, /className="panel-card/);
  assert.doesNotMatch(source, /className="card"/);
});
