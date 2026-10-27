import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const read = name => readFileSync(new URL(name, import.meta.url), "utf8");

test("ambiguous visible candidates expose bounded structural evidence without text or mutations", () => {
  const native = read("../../src-tauri/src/codex_composer_macos.rs");
  const evidence = native.split("fn candidate_structure(")[1].split("fn diagnostic_composer_counts(")[0];
  assert.match(evidence, /take\(8\)/);
  assert.match(evidence, /0\.\.MAX_TREE_DEPTH/);
  for (const field of ["relativeBounds", "ancestorCandidateIndices", "sharedAncestorDepths", "hitTests", "reachesWindow"]) {
    assert(evidence.includes(`"${field}"`));
  }
  assert.doesNotMatch(evidence, /\.press\(|set_attribute|post_keyboard|element_labels|element_value|"value"\s*:|"title"\s*:/);
  assert.match(read("composer-diagnostic.jsx"), /不用先点击 Codex 输入框/);
});

test("voice failures preserve the same selection candidates, with bounded work and no persistence", () => {
  const native=read("../../src-tauri/src/codex_composer_macos.rs");
  const selector=native.split("fn select_window_composer(")[1].split("fn find_target(")[0];
  assert.match(selector,/candidate_structure\(window, &elements\)/);
  assert.match(selector,/if slot\.is_none\(\)/);
  assert.doesNotMatch(selector,/diagnostic_composer_counts|fs::|clipboard/);
  const evidence=native.split("fn candidate_structure(")[1].split("fn diagnostic_composer_counts(")[0];
  assert.match(evidence,/Duration::from_millis\(300\)/);
  assert.doesNotMatch(evidence,/composer_candidates\(/);
  assert.match(native,/impl Drop for ComposerEvidenceScope/);
  assert.match(read("VoiceAssistantPanel.jsx"),/失败时的定位详情/);
});

test("unfocused editor recovery uses read-only surface evidence, not a click or first candidate", () => {
  const native = read("../../src-tauri/src/codex_composer_macos.rs");
  const probe = native.split("fn nearest_hit_text_control(")[1].split("fn select_window_composer(")[0];
  assert.match(probe, /AXUIElementCopyElementAtPosition/);
  assert.doesNotMatch(probe, /\.press\(|post_keyboard|set_attribute|CGEvent::/);
  const selector = native.split("fn select_window_composer(")[1].split("fn find_target(")[0];
  assert.match(selector, /composer_visibility::may_auto_focus/);
  assert.match(selector, /composer_visibility::sole_exposed\(&exposures\)/);
  assert.match(selector, /AXFocusedUIElement"\) == focused/);
  assert.match(selector, /\(2\.\.=8\)\.contains/);
  const start = native.split("fn find_stable_current_target(")[1].split("fn find_voice_target(")[0];
  assert.match(start, /focus_composer\(&candidate\)\?/);
  assert.match(start, /refreshed_composer != composer/);
});

test("diagnostic checks production resolver and distinguishes readiness from real voice acceptance", () => {
  const native = read("../../src-tauri/src/codex_composer_macos.rs");
  const check = native.split("pub(super) fn diagnose_current_composer(")[1].split("fn find_stable_current_target(")[0];
  assert.match(check, /find_composer\(agent, &windows\[\*index\]\)/);
  assert.match(check, /"voiceSelection":selection/);
  assert.match(read("VoiceComposerCheck.jsx"), /result\.voiceSelection\.ok/);
  assert.match(read("VoiceComposerCheck.jsx"), /仍需真实语音验收/);
});

test("composer readiness diagnostic never records, writes, submits or calls an Agent", () => {
  const ui = read("VoiceComposerCheck.jsx");
  assert.match(ui, /只检查输入框（不写入、不发送）/);
  assert.match(ui, /await invoke\("check_voice_composer_readiness", \{ agentId \}\)/);
  assert.equal((ui.match(/invoke\(/g) || []).length, 1);
  assert.match(ui, /sequence\.current === current/);
  const lib = read("../../src-tauri/src/lib.rs");
  const command = lib.split("async fn check_voice_composer_readiness(")[1].split("async fn write_workbuddy_voice_test_draft(")[0];
  assert.match(command, /active\.is_some\(\)/);
  assert.match(command, /composer_gate\(\)/);
  assert.match(command, /diagnose_current_composer\(&agent_id\)/);
  assert.doesNotMatch(command, /\.update\(|\.confirm\(|\.dispatch\(|StreamingSpeechRecognizer|postMockButtonInject/);
});

test("readiness check is explicit and restricted to local UI, with no draft content in scan diagnostics", () => {
  const permissions = JSON.parse(read("../../src-tauri/permissions/local-app-commands.json"));
  assert(permissions.permission[0].commands.allow.includes("check_voice_composer_readiness"));
  const native = read("../../src-tauri/src/codex_composer_macos.rs");
  const check = native.split("pub(super) fn diagnose_current_composer(")[1].split("fn find_stable_current_target(")[0];
  assert.match(check, /legacy_codex_window_index/);
  assert.match(check, /codex_primary_window_index/);
  assert.match(check, /diagnostic_composer_counts/);
  assert.doesNotMatch(check, /begin_voice|update_voice|confirm_voice|cancel_voice|launch_agent_application|enable_agent_accessibility/);
  assert.match(native, /should_recover_workbuddy_tree\(agent, candidates\.len\(\), recovery_attempted\)/);
  assert.match(native, /refreshed_composer != composer/);
  assert.match(native, /current\.window != pinned_window/);
});

test("diagnostic build skips all automatic USB, credentials, audio and shared-runtime shutdown", () => {
  const lib=read("../../src-tauri/src/lib.rs");
  const start=lib.split("pub fn run() {")[1];
  assert.match(start, /if !cfg!\(feature = "composer-diagnostic"\) \{[\s\S]*start_usb_host_heartbeat/);
  const setup=start.split(".setup(move |app| {")[1].split("desktop_window::fit")[0];
  assert.match(setup, /cfg!\(feature = "composer-diagnostic"\)/);
  assert.match(setup, /return Ok\(\(\)\)/);
  assert.doesNotMatch(setup, /configure_storage|start_usb|load\(/);
  const exit=start.split("app.run(|app_handle, event| {")[1];
  assert.match(exit, /if cfg!\(feature = "composer-diagnostic"\) \{ return; \}/);
  assert.match(read("composer-diagnostic.jsx"), /不运行尚未证实的候选修复/);
});

test("detailed diagnostic exposes categorical controls without labels or draft text", () => {
  const native = read("../../src-tauri/src/codex_composer_macos.rs");
  const scan = native.split("fn diagnostic_composer_counts(")[1].split("pub(super) fn diagnose_current_composer(")[0];
  const output = scan.split("text_controls.push(serde_json::json!({")[1].split("}));")[0];
  assert.match(output, /"valueSettable"/);
  assert.match(output, /"isFocusedElement"/);
  assert.match(output, /"domKindHints":kinds/);
  assert.doesNotMatch(output, /"(?:value|labels|title|identifier|selectedText|dom)"\s*:/);
  assert.match(scan, /"focusedInScan":focused_in_scan/);
  assert.match(read("VoiceComposerCheck.jsx"), /第 \{index \+ 1\} 次检查/);
});
