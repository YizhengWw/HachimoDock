import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const rust = (name) => readFileSync(new URL(`../src-tauri/src/${name}`, import.meta.url), "utf8");

test("realtime delegation targets the current editor independently of display-follow bindings", () => {
  const source = rust("desktop_agent.rs");
  assert.match(source, /CodexComposerBridge::start_current_task_for\(owned.agent.id\(\)\)/);
  assert.doesNotMatch(source, /ctx\.target|target_current\(/);
  assert.match(source, /无需绑定设备跟随会话/);
  assert.match(source, /reserve\(&mut store, ctx, ""\)/);
  assert.match(source, /let \(agent, _\) = crate::resolve_usb_inject_target\(&self.board\)/);
  assert.match(source, /if !ctx.supports_default_agent\(\)/);
  assert.match(rust("realtime_chat.rs"), /with_followed_agent\(\)/);
});

test("macOS current-task lookup never launches or navigates and freezes editor identity", () => {
  const source = rust("codex_composer_macos.rs");
  const start = source.slice(source.indexOf("fn current_task_window()"), source.indexOf("pub(super) fn update_voice"));
  assert.doesNotMatch(start, /launch_agent_application|primary_or_launch|deep_link|focus_session\(/);
  assert.match(start, /current_task_window_index\(&metadata\)/);
  assert.match(source, /subrole == "AXStandardWindow"/);
  assert.match(source, /DESKTOP_TASK_MODAL_OPEN/);
  assert.match(start, /task_identity: Some\(identity\)/);
  assert.match(source, /if state.task_identity.is_some\(\) \{\s+return Err\("DESKTOP_TASK_TARGET_CHANGED"/);
  assert.match(source, /press_unique_submission\(&target, true\)/);
  assert.match(source, /let mode=task_composer_stage\("submit_button", \|\| \{[\s\S]*submit_once\([\s\S]*ok_or_else\(\|\| "DESKTOP_TASK_SEND_UNAVAILABLE"/);
});

test("Windows current-task lookup rejects ambiguous windows without using launch/navigation", () => {
  const source = rust("codex_composer.rs");
  const start = source.slice(source.indexOf("function Get-CurrentTaskTarget"), source.indexOf("function Assert-TargetCurrent"));
  assert.match(start, /Get-CodexWindows/);
  assert.match(start, /\$windows.Count -ne 1/);
  assert.doesNotMatch(start, /Get-OrLaunch|Open-CodexSession|Start-Process/);
  assert.match(source, /\$purpose -eq 'current_task'[\s\S]*Get-CurrentTaskTarget \$null/);
  assert.match(source, /Get-TaskWindowIdentity \$current.Root/);
  assert.match(source, /if \(\$checkDraft -and \$currentValue -cne \$state.LastValue\)/);
  const dispatch=source.split("'dispatch' {")[1].split("'release' {")[0];
  assert.match(dispatch, /DESKTOP_TASK_READBACK_CHANGED/);
  assert.equal((dispatch.match(/Invoke-SendButton/g) || []).length,1);
  assert.match(dispatch, /submission = \$submission/);
  assert.doesNotMatch(dispatch, /PressEnter|Start-Sleep|readbackDeadline/);
  assert.match(source, /if \(\$strictButton -or -not \$allowQueue\) \{ throw 'DESKTOP_TASK_SEND_UNAVAILABLE' \}/);
  assert.match(source, /\$submission = Invoke-SendButton \$state \$true \$true/);
});

test("Windows ChatGPT task clicks the validated button once without an Invoke retry", () => {
  const source = rust("codex_composer.rs");
  const click = source.split("function Invoke-CodexTaskSubmit")[1].split("function Invoke-SendButton")[0];
  assert.match(click, /Assert-TargetCurrent \$state \$true/);
  assert.match(click, /Test-CodexSubmitPoint \$state \$button \$x \$y/);
  assert.match(click, /DESKTOP_TASK_READBACK_CHANGED/);
  assert.equal((click.match(/::ClickSubmit\(/g) || []).length, 1);
  assert.doesNotMatch(click, /\.Invoke\(|PressEnter|catch\s*\{/);
  const submit = source.split("function Invoke-SendButton")[1].split("$state = $null")[0];
  assert.match(submit, /\$nativeTaskClick = \$strictButton -and \$state.CurrentTask -and \$state.Agent -eq 'codex'/);
  assert.match(submit, /if \(\$nativeTaskClick\) \{\s+Invoke-CodexTaskSubmit \$state \$buttons\[0\]\s+\} else \{\s+\$buttons\[0\].Pattern.Invoke\(\)/);
  const native = source.split("public static void ClickSubmit")[1].split("// WorkBuddy")[0];
  assert.match(native, /GetForegroundWindow\(\) != window/);
  assert.match(native, /IsOwnedWindow\(window, WindowFromPoint\(point\)\)/);
  assert.ok(native.indexOf("DESKTOP_TASK_TARGET_CHANGED") < native.indexOf("Click(x, y)"));
});

test("macOS task and voice share focused-or-bottom composer selection and retain task guards", () => {
  const source = rust("codex_composer_macos.rs");
  const task = source.split("pub(super) fn begin_current_task_for(")[1].split("pub(super) fn update_voice")[0];
  assert.match(task, /find_stable_current_target\(agent, true\)/);
  const voice = source.split("fn find_current_visible_target(")[1].split("fn find_stable_current_target")[0];
  assert.match(voice, /find_stable_current_target\(agent, false\)/);
  const ready = source.split("fn find_stable_current_target(")[1].split("fn find_voice_target")[0];
  assert.match(ready, /if current_task \{\s+current_task_window_for\(agent\)\?/);
  assert.match(ready, /current.window != pinned_window/);
  assert.match(ready, /COMPOSER_STABILITY_TIMEOUT/);
  assert.match(ready, /should_recover_workbuddy_tree\(agent, candidates.len\(\), recovery_attempted\)/);
  assert.match(ready, /recovery_attempted = true/);
  assert.match(ready, /let focused = focused_codex_composer\(agent, &target\);/);
  assert.doesNotMatch(ready, /if current_task \{ None \}|unique_composer\(agent, candidates\)/);
  assert.match(ready, /select_window_composer\(agent, &target, candidates\)/);
  assert.ok(ready.indexOf("focused_codex_composer") < ready.indexOf("composer_candidates"));
  const selector = source.split("fn select_window_composer(")[1].split("fn find_target(")[0];
  assert.match(selector, /focused_composer_index/);
  assert.match(selector, /composer_visibility::stacked_chat_composer/);
  assert.match(ready, /refreshed_composer != composer/);
  assert.match(ready, /refreshed_value != candidate.value/);
  assert.doesNotMatch(ready, /replace_composer_text|post_workbuddy_key|press_unique_submission/);
});

test("both platforms append live drafts without waiting for old receipts", () => {
  const coordinator = rust("desktop_agent.rs");
  assert.doesNotMatch(coordinator, /cfg!?\(windows\)/);
  const append = coordinator.split("fn prepare_draft_append")[1].split("async fn run_dispatch")[0];
  assert.match(append, /handoff_payload\(&ctx.history, &ctx.request, Some\(text\)\)/);
  assert.match(append, /\.hash = request_hash\(&payload\)/);
  assert.doesNotMatch(coordinator, /resume_from|State::Continued/);
  assert.doesNotMatch(append, /previous\.hash|previous\.request_hash/);
  const script = rust("codex_composer.rs");
  const dispatch = script.split("'dispatch' {")[1].split("'release' {")[0];
  assert.match(dispatch, /Assert-TaskDraftSnapshot \$state \$command.draftSnapshot/);
  assert.match(dispatch, /\$state.BaseValue = ''/);
  assert.match(dispatch, /elseif \(-not \[string\]::IsNullOrEmpty\(\$state.BaseValue\)/);
  const queue = coordinator.split("fn earlier_input_pending")[1].split("async fn run_dispatch")[0];
  assert.match(queue, /State::Preparing \| State::Sending/);
  assert.doesNotMatch(queue, /State::Unknown|State::Confirming|State::Working/);
  const reservation = coordinator.split("fn reserve(")[1].split("fn task(")[0];
  assert.doesNotMatch(reservation, /ctx.agent != Agent::Codex/);
  const run = coordinator.split("async fn run_dispatch(")[1].split("fn safe_excerpt(")[0];
  assert.doesNotMatch(run, /(?:ctx|owned).agent == Agent::Codex/);
  assert.match(run, /bridge.inspect_task_draft\(\)/);
  assert.match(run, /bridge.dispatch_task_from_draft\(&handoff, &snapshot\)/);
  const bridge = script.split("impl CodexComposerBridge {")[1].split("/// Success means")[0];
  assert.match(bridge, /pub fn inspect_task_draft/);
  assert.match(bridge, /pub fn dispatch_task_from_draft/);
  assert.doesNotMatch(bridge, /#\[cfg\(windows\)\]\s+pub fn (?:inspect_task_draft|dispatch_task_from_draft)/);
});

test("WorkBuddy snapshots share the safe task protocol but keep native adapters isolated", () => {
  const windows = rust("codex_composer.rs");
  const snapshot = windows.split("function Get-TaskDraftSnapshot")[1].split("function Test-CodexSubmitPoint")[0];
  assert.match(snapshot, /@\('codex','workbuddy'\) -notcontains \$state.Agent/);
  assert.match(snapshot, /agent = \$state.Agent/);
  assert.match(snapshot, /@\('agent','processId'/);
  const dispatch = windows.split("'dispatch' {")[1].split("'release' {")[0];
  assert.doesNotMatch(dispatch, /\$state.Agent -eq 'codex'/);
  assert.match(windows, /\$attempts = if \(\$state.Agent -eq 'workbuddy'\) \{ 1 \}/);
  // Do not substitute Codex's managed UIA hit testing for WorkBuddy's native provider.
  assert.match(windows, /\$nativeTaskClick = \$strictButton -and \$state.CurrentTask -and \$state.Agent -eq 'codex'/);
  const mac = rust("codex_composer_macos.rs");
  const macSnapshot = mac.split("fn task_draft_snapshot(")[1].split("pub(super) fn inspect_task_draft")[0];
  assert.match(macSnapshot, /MacosAgent::Codex \| MacosAgent::WorkBuddy/);
  assert.match(macSnapshot, /"agent":state.agent.label\(\)/);
  const writer = mac.split("fn replace_composer_text(")[1].split("pub(super) fn begin_voice")[0];
  assert.match(writer, /target.agent == MacosAgent::WorkBuddy/);
  assert.match(writer, /select_workbuddy_draft/);
  assert.match(writer, /snapshot.restore\(\)/);
});

test("macOS validates the live draft then uses one native submission without trimming or reappending", () => {
  const source = rust("codex_composer_macos.rs");
  const dispatch = source.split("pub(super) fn dispatch_task_from_draft(")[1].split("pub(super) fn dispatch_voice_task")[0];
  assert.match(dispatch, /task_composer_stage\("target_refresh", \|\| find_voice_target\(state\)\)\?/);
  assert.match(dispatch, /expected.get\("target"\) != current.get\("target"\)/);
  assert.match(dispatch, /expected.get\("text"\) != current.get\("text"\)/);
  assert.match(dispatch, /task_composer_stage\("draft_write", \|\| \{/);
  assert.match(dispatch, /target.agent == MacosAgent::Codex.*replace_codex_task_text\(&target, text\)/);
  assert.match(dispatch, /else \{ replace_composer_text\(&target, text\) \}/);
  assert.match(dispatch, /DESKTOP_TASK_WRITE_NOT_CONFIRMED/);
  assert.match(dispatch, /confirm_voice_inner\(state, Some\(text\)\)/);
  assert.match(dispatch, /state.agent == MacosAgent::Codex.*submit_codex_task_after_paste\(state\)/);
  assert.doesNotMatch(dispatch, /update_voice\(|\.desired\(|\.trim\(|press_enter\(/);
  assert.equal((dispatch.match(/replace_composer_text\(/g) || []).length, 1);
  const worker = rust("codex_composer.rs").split('"inspect_task_draft" =>')[1].split('"confirm" =>')[0];
  assert.match(worker, /macos::inspect_task_draft/);
  assert.match(worker, /macos::dispatch_task_from_draft\(state, &text, snapshot\)/);
});

test("Codex task uses the voice native paste transport once and preserves safe replacement", () => {
  const source=rust("codex_composer_macos.rs");
  const write=source.split("fn replace_codex_task_text(")[1].split("fn task_composer_stage")[0];
  assert.match(write,/post_codex_command\(&target.app, 0\)\?/);
  assert.doesNotMatch(write,/AXSelectedText|encode_utf16|codex_task_selection_matches/);
  assert.match(write,/AXFocusedUIElement/);
  assert.match(write,/assert_unchanged\(\)\?;\s+post_codex_paste/);
  assert.match(write,/snapshot.restore\(\)/);
  assert.equal((write.match(/post_codex_paste\(/g)||[]).length,1);
  assert.doesNotMatch(write,/AXAttribute::value\(|press_enter\(|press_unique_submission\(/);
  const afterPaste=write.split("post_codex_paste(&target.app)?;")[1];
  assert.doesNotMatch(afterPaste,/composer_value\(|deadline|loop\s*\{|text\s*==|==\s*text/);
  const immediateSubmit=source.split("fn submit_codex_task_after_paste(")[1].split("pub(super) fn dispatch_task_from_draft(")[0];
  assert.match(immediateSubmit,/find_voice_target\(state\)\?/);
  assert.match(immediateSubmit,/submit_once\(/);
  assert.match(immediateSubmit,/press_native_submission\(&target, true, false\)/);
  assert.match(immediateSubmit,/assert_submission_focus\(&target\)\?/);
  assert.doesNotMatch(immediateSubmit,/assert_submission_target/);
  assert.doesNotMatch(immediateSubmit,/target.value|expected|\.len\(|chars\(|wait_for_submit_readback|sleep\(/);
  const transport=source.split("fn post_codex_paste(")[1].split("fn insert_codex_voice_once(")[0];
  assert.match(transport,/CGEventSourceStateID::Private/);
  assert.match(transport,/down.post_to_pid\(pid\); up.post_to_pid\(pid\);/);
  const coordinator=rust("desktop_agent.rs").split("let submitted = bridge.dispatch_task_from_draft")[1].split("let drained")[0];
  assert.match(coordinator,/if error.starts_with\("DESKTOP_TASK_WRITE_NOT_CONFIRMED:"\)[\s\S]*?State::Failed/);
  const submit=source.split('if expected.is_some() {')[1].split('return Ok(mode.into())')[0];
  assert.match(submit,/state.agent == MacosAgent::Codex[\s\S]*submit_once\(/);
  assert.match(submit,/assert_submission_target\(&target\)\?; press_enter\(target.agent, &target.app\)/);
});

test("completed voice ownership yields without clearing or submitting its draft", () => {
  const source = rust("lib.rs");
  const release = source.split("fn release_completed_voice_for_desktop_task(")[1].split("fn submit_device_voice_via_agent_bus")[0];
  assert.match(release, /completed_voice_can_yield_to_task/);
  assert.match(release, /draft_submit_requested.compare_exchange\(false, true/);
  assert.match(release, /supersede_device_voice_context\(&context\)/);
  assert.doesNotMatch(release, /\.cancel\(|\.submit\(|dispatch_task\(|update_confirmed\(/);
  const dispatch = rust("desktop_agent.rs");
  assert.match(dispatch, /release_completed_voice_for_desktop_task\(&owned.board\)/);
  assert.match(dispatch, /DESKTOP_TASK_DRAFT_NOT_EMPTY/);
  assert.match(dispatch, /"stage":"preflight", "reason":reason/);
  assert.match(dispatch, /State::Failed if !task.seen_user/);
});
