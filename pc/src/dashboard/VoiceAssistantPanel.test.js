/**
 * [Input] Read VoiceAssistantPanel.jsx source.
 * [Output] Static coverage for compact voice controls, immediate ASR-change rearming, ChatGPT（Codex）/Claude-visible and MiMoCode-caret labels, macOS/Windows recovery paths, trust checks, diagnostics, and summaries.
 * [Pos] test node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
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

test("Exposes the voice controls while session switching stays in button actions", () => {
  assert.match(source, /import Switch from "\.\.\/shell\/Switch"/);
  assert.match(source, /voice-panel__command/);
  assert.match(source, /按键语音已启用/);
  assert.match(source, /voice-panel__command-switch/);
  assert.doesNotMatch(source, /voice-session-select/);
  assert.match(source, /启动语音监听/);
  assert.match(source, /停止语音监听/);
});

test("Reads Volcengine ASR readiness and delegates credential editing to API settings", () => {
  assert.match(source, /load_device_asr_settings/);
  assert.match(source, /onCredentialReady/);
  assert.match(source, /onOpenApiSettings/);
  assert.match(source, /type: "set_voice_runtime"/);
  assert.match(source, /API_CONFIGURATION_UPDATED_EVENT/);
  assert.match(source, /语音识别 API/);
  assert.match(source, /前往 API 配置/);
  assert.doesNotMatch(source, /type="password"/);
  assert.doesNotMatch(source, /save_device_asr_settings|test_device_asr_settings/);
  assert.doesNotMatch(source, /status\?\.apiKey/);
});

test("Rearms device voice only after a saved Volcengine ASR update", () => {
  assert.match(source, /const loadAsrState = \(\{ resume = false \} = \{\}\) =>/);
  assert.match(source, /if \(resume && status\?\.configured === true\)/);
  assert.match(source, /event\?\.detail\?\.providerId && event\.detail\.providerId !== "volcengine-asr"/);
  assert.match(source, /loadAsrState\(\{ resume: true \}\)/);
  assert.match(source, /addEventListener\(API_CONFIGURATION_UPDATED_EVENT, handleApiConfigurationUpdated\)/);
  assert.match(source, /removeEventListener\(API_CONFIGURATION_UPDATED_EVENT, handleApiConfigurationUpdated\)/);
});


test("Shows mock text injection and board voice action result status", () => {
  assert.match(source, /sendMockButtonInject/);
  assert.match(source, /诊断与测试/);
  assert.match(source, /模拟文本注入/);
  assert.match(source, /发送到当前会话/);
  assert.match(source, /模型回复预览/);
  assert.match(source, /deviceVoiceFlow/);
  assert.match(source, /设备语音状态/);
  assert.match(source, /发送中/);
  assert.match(source, /等待回复/);
  assert.match(source, /实时识别文本/);
  assert.match(source, /设备语音回复预览/);
});

test("Shows live P4 device microphone relay and recognition status", () => {
  const dashboard = readFileSync(join(here, "useDeviceVoiceRouter.js"), "utf8");

  assert.match(dashboard, /listen\("usb-audio-stream"/);
  assert.match(dashboard, /listen\("voice-transcript"/);
  assert.match(dashboard, /type: "transcript"/);
  assert.match(dashboard, /composerMode/);
  assert.match(dashboard, /设备麦克风录音中/);
  assert.match(dashboard, /设备录音完成/);
  assert.match(source, /audioBridgeMessage/);
  assert.match(dashboard, /正在识别设备麦克风录音/);
  assert.doesNotMatch(dashboard, /PC 默认麦克风/);
  assert.match(source, /启动语音监听/);
  assert.match(source, /实时识别文本/);
  assert.match(source, /\{visibleVoiceAgentLabel\} 可见同步/);
  assert.match(source, /visibleVoiceAgentId === "claude-code"[\s\S]*?"Claude"[\s\S]*?"ChatGPT（Codex）"/);
  assert.match(source, /MiMoCode 光标草稿/);
  assert.doesNotMatch(source, /后台回退|会话桥接/);
  assert.match(source, /composerMode === "visible"/);
  assert.match(source, /composerMode === "focused-input"/);
  assert.match(source, /isMimocodeVoice/);
  assert.match(source, /保持 MiMoCode 终端在前台/);
  assert.match(source, /短按确认键（默认 SW3）才发送/);
});

test("Guides ChatGPT（Codex） and Claude foreground failures with macOS and Windows Dev tips", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  const tauri = readFileSync(join(here, "..", "..", "src-tauri", "src", "lib.rs"), "utf8");

  assert.match(source, /export const needsMacosAccessibilityGuidance/);
  assert.match(source, /export function needsVisibleComposerGuidance/);
  assert.match(source, /export function detectDesktopPlatform/);
  assert.match(source, /系统设置 → 隐私与安全性 → 辅助功能/);
  assert.match(source, /请求系统授权/);
  assert.match(source, /macOS 弹窗中点击“打开系统设置”/);
  assert.match(source, /重新检查/);
  assert.match(source, /pc\/src-tauri\/target\/debug\/pet-manager-tauri/);
  assert.match(source, /pc\\src-tauri\\target\\debug\\pet-manager-tauri\.exe/);
  assert.match(source, /Windows 不需要把 Pet Manager 添加到“辅助功能”列表/);
  assert.match(source, /相同权限级别/);
  assert.match(source, /⌘⇧G/);
  assert.match(source, /invoke\("check_codex_accessibility_permission"\)/);
  assert.match(source, /invoke\("request_codex_accessibility_permission"\)/);
  const requestAccessibility = source.match(
    /const requestAccessibilityPermission[\s\S]*?const recheckAccessibilityPermission/,
  );
  assert.ok(requestAccessibility, "expected explicit native Accessibility authorization action");
  assert.match(requestAccessibility[0], /invoke\("request_codex_accessibility_permission"\)/);
  assert.doesNotMatch(requestAccessibility[0], /open_macos_accessibility_settings/);
  const recheckAccessibility = source.match(
    /const recheckAccessibilityPermission[\s\S]*?\n  };/,
  );
  assert.ok(recheckAccessibility, "expected non-prompting Accessibility status retry action");
  assert.match(recheckAccessibility[0], /invoke\("check_codex_accessibility_permission"\)/);
  assert.doesNotMatch(recheckAccessibility[0], /request_codex_accessibility_permission/);
  assert.match(source, /请不要再次添加权限或重复输入系统密码/);
  assert.match(css, /\.voice-panel__accessibility-guide\s*\{/);
  assert.match(css, /\.voice-panel__accessibility-guide\.is-trusted\s*\{/);
  assert.match(tauri, /fn check_codex_accessibility_permission\(\)/);
  assert.match(tauri, /fn request_codex_accessibility_permission\(\)/);
  assert.doesNotMatch(tauri, /fn open_macos_accessibility_settings\(\)/);
  assert.doesNotMatch(tauri, /Privacy_Accessibility/);
  assert.match(tauri, /check_codex_accessibility_permission,/);
  assert.match(tauri, /request_codex_accessibility_permission,/);
});

test("Uses shared form controls instead of one-off native-looking fields", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");

  assert.match(source, /ui-field/);
  assert.match(source, /ui-control ui-control--textarea/);
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

test("Combines primary status and actions while pairing settings and diagnostics", () => {
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");

  assert.match(source, /voice-panel__command/);
  assert.doesNotMatch(source, /voice-panel__overview|voice-panel__runtime/);
  assert.match(source, /voice-panel__advanced-grid/);
  assert.match(source, /<details[\s\S]*voice-panel__advanced/);
  assert.match(source, /voice-panel__advanced--diagnostics/);
  assert.match(source, /formatVoiceUserMessage/);
  assert.match(source, /设备未收到语音监听指令，请确认 USB 连接后重试/);
  assert.match(source, /draft_ready: "草稿待确认"/);
  assert.match(source, /短按确认键（默认 SW3）才发送/);
  assert.match(css, /\.voice-panel__command\s*\{/);
  assert.match(css, /\.voice-panel__advanced-grid\s*\{/);
  assert.match(css, /\.voice-panel__advanced-summary\s*\{/);
  assert.match(css, /\.voice-panel__advanced\[open\]/);
  assert.match(css, /@media \(max-width: 820px\)/);
  assert.match(css, /@media \(max-width: 560px\)/);
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
