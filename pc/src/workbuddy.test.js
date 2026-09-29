import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { defaultAgentAppearanceMap, normalizeDetectedAgents, sanitizeAgentAppearanceMap } from "./lib/agent-appearance-config.js";
const read = p => readFileSync(new URL(p, import.meta.url), "utf8");
test("WorkBuddy has an independent appearance binding without changing existing agents", () => {
  const records = [{ id: "builtin-terrier" }, { id: "custom" }];
  const defaults = defaultAgentAppearanceMap(records);
  assert.equal(defaults.workbuddy, "builtin-terrier");
  const bound = sanitizeAgentAppearanceMap({ ...defaults, workbuddy: "custom" }, records);
  assert.equal(bound.workbuddy, "custom"); assert.equal(bound.codex, defaults.codex);
  const agents = normalizeDetectedAgents([{ id: "workbuddy", detected: true }]);
  assert.equal(agents.at(-1).label, "WorkBuddy"); assert.equal(agents.at(-1).detected, true);
});
test("WorkBuddy stays isolated through native USB routing and Bridge startup", () => {
  const rust = read("../src-tauri/src/lib.rs");
  assert.match(rust, /KNOWN_USB_STATE_SOURCES[^;]+"workbuddy"/);
  assert.match(rust, /"workbuddy" => "workbuddy"\.to_string\(\)/);
  assert.match(rust, /detect_workbuddy\(\),/);
  const bridge = read("../src-tauri/bridge/packages/clawd-backend-service/src/headless-mqtt.js");
  assert.match(bridge, /syncWorkBuddy: config.workbuddy.enabled/);
  assert.match(bridge, /syncWorkBuddy: options.syncWorkBuddy === true/);
  assert.match(bridge, /parseCsvEnv\("CLAWD_ENABLED_AGENTS", \[\]\).includes\("workbuddy"\)/);
  assert.match(bridge, /new WorkBuddyAdapter/);
});
test("WorkBuddy voice uses its own current desktop composer and never falls back to Codex", () => {
  const source = read("../src-tauri/src/lib.rs").split("fn start_device_voice_context(")[1].split("fn ")[0];
  assert.doesNotMatch(source, /暂不支持按键语音输入/);
  assert.match(source, /if startup_context.target.agent_id == "workbuddy" \{\s*Err\(/);
  const composer = read("../src-tauri/src/codex_composer.rs");
  assert.match(composer, /"workbuddy" => macos::MacosAgent::WorkBuddy/);
  assert.match(composer, /"workbuddy" => &\["workbuddy.exe"\]/);
  // Every WorkBuddy write re-verifies the bound composer identity, ownership,
  // writability and exact last text without repeating the full window lookup.
  assert.match(composer, /\$state.CurrentVisible -and \$state.Agent -eq 'workbuddy'\) \{\s*Assert-WorkBuddyComposerCurrent \$state/);
  const recheck = composer.split("function Assert-WorkBuddyComposerCurrent(")[1].split("function Assert-TargetCurrent(")[0];
  assert.match(recheck, /\$runtimeId -ne \$state.ComposerRuntimeId/);
  assert.match(recheck, /Test-WorkBuddyElementOwner/);
  assert.match(recheck, /Test-WorkBuddyWritablePattern/);
  assert.match(recheck, /IsForeground/);
  assert.doesNotMatch(recheck, /Get-WorkBuddyTarget|Start-Sleep|FindBounded/);
  const assertTarget = composer.split("function Assert-TargetCurrent(")[1].split("function Focus-Composer(")[0];
  assert.match(assertTarget, /\$currentValue -cne \$state.LastValue/);
  assert.doesNotMatch(read("./dashboard/VoiceAssistantPanel.jsx"), /暂不支持按键语音输入/);
  assert.match(read("../src-tauri/src/lib.rs"), /agent: "WorkBuddy",\s*home_dir: "\.workbuddy"/);
});

test("WorkBuddy diagnostic uses the actual draft writer and never submits", () => {
  const source = read("../src-tauri/src/lib.rs").split("async fn write_workbuddy_voice_test_draft(")[1].split("#[tauri::command]")[0];
  assert.match(source, /start_current\("workbuddy"/);
  assert.match(source, /update_confirmed/);
  assert.match(source, /preserve_draft/);
  assert.doesNotMatch(source, /\.confirm\(/);
  assert.match(read("./DeviceDashboard.jsx"), /invoke\("write_workbuddy_voice_test_draft"/);
  assert.match(read("./dashboard/VoiceAssistantPanel.jsx"), /只写入 WorkBuddy 草稿/);
});

test("Windows WorkBuddy waits for Chromium roots without changing the generic locator", () => {
  const source = read("../src-tauri/src/codex_composer.rs");
  const workbuddy = source.split("function Get-WorkBuddyTarget(")[1].split("function Get-CurrentVisibleTarget(")[0];
  assert.match(workbuddy, /Start-Sleep -Milliseconds 120/);
  assert.match(workbuddy, /\+ 4500/);
  assert.match(workbuddy, /Chrome_RenderWidgetHostHWND/);
  assert.match(workbuddy, /\[WorkBuddyUia\]::FindBounded\(\$handle, \$true, 1500, 3500\)/);
  assert.match(workbuddy, /Test-WorkBuddyElementOwner \$root \$composer.Element/);
  assert.match(workbuddy, /IsForeground/);
  assert.doesNotMatch(workbuddy, /ReplaceFocusedText|PressEnter|::Click\(/);
  assert.match(source, /return Get-WorkBuddyTarget \$allowedValues/);
});

test("Windows WorkBuddy requires writable owned editors and rejects ambiguity", () => {
  const source = read("../src-tauri/src/codex_composer.rs");
  const filter = source.split("function Get-ComposerCandidates(")[1].split("function Find-Composer(")[0];
  assert.match(filter, /\[bool\]\$workbuddy = \$false/);
  assert.match(filter, /Test-WorkBuddyWritablePattern/);
  assert.match(filter, /Test-WorkBuddyElementOwner/);
  assert.match(filter, /searchbox/);
  assert.match(filter, /-not \$documentHint\) \{ continue \}/);
  const select = source.split("function Select-WorkBuddyComposer(")[1].split("function Get-WorkBuddyTarget(")[0];
  assert.match(select, /Get-ElementRuntimeId/);
  assert.match(select, /\$distinct.Count -gt 1/);
  assert.doesNotMatch(select, /Sort-Object|BoundsKey/);
});

test("Windows WorkBuddy checks semantic keyboard focus before bounded tree and point scans", () => {
  const source = read("../src-tauri/src/codex_composer.rs");
  const target = source.split("function Get-WorkBuddyTarget(")[1].split("function Get-CurrentVisibleTarget(")[0];
  assert.ok(target.indexOf("Find-WorkBuddyFocusedComposer") < target.indexOf("[WorkBuddyUia]::FindBounded"));
  assert.ok(target.indexOf("[WorkBuddyUia]::FindBounded") < target.indexOf("[WorkBuddyUia]::FindAtPoints"));
  assert.match(target, /\[WorkBuddyUia\]::FindAtPoints\(\$handle, [^\n]+, 250\)/);
  assert.match(target, /Write-ComposerProgress 'validate'/);
  const native = source.split("public static class WorkBuddyUia {")[1].split('"@')[0];
  const points = native.split("public static WorkBuddyUiaElement[] FindAtPoints(")[1];
  assert.match(points, /clock.ElapsedMilliseconds >= budgetMs/);
  assert.match(points, /depth < 16/);
  assert.match(source, /composer_startup_timeout_message\(agent, stage\)/);
  assert.match(source, /read_windows_composer_response\(&mut stdout/);
});

test("Windows WorkBuddy initializes only owned accessibility windows and re-reads the tree", () => {
  const source = read("../src-tauri/src/codex_composer.rs");
  const native = source.split("public static int RequestClientAccessibility(")[1].split('[DllImport("user32.dll", SetLastError = true)]')[0];
  assert.match(native, /processId != \(uint\)expectedProcessId/);
  assert.match(native, /IsForeground\(parent\)/);
  assert.match(native, /IsOwnedWindow\(parent, window\)/);
  assert.match(native, /Chrome_RenderWidgetHostHWND/);
  assert.match(native, /0x003D.*new IntPtr\(1\)/);
  assert.match(native, /250, out result/);
  assert.match(native, /AccessibleObjectFromWindow\(window, 0xFFFFFFFC/);
  assert.match(native, /finally[\s\S]*Marshal.Release\(accessible\)/);
  const initialize = source.split("function Initialize-WorkBuddyAccessibility(")[1].split("function Get-WorkBuddyTarget(")[0];
  assert.match(initialize, /WorkBuddyAccessibilityRequests.ContainsKey/);
  assert.match(initialize, /Renderers -ge 4/);
  assert.match(initialize, /Get-MonotonicMilliseconds\) -ge \$deadline/);
  assert.doesNotMatch(initialize, /ReplaceFocusedText|PressEnter|::Click\(|Start-Process|Stop-Process|SystemParametersInfo/);
  const target = source.split("function Get-WorkBuddyTarget(")[1].split("function Get-CurrentVisibleTarget(")[0];
  assert.match(target, /Initialize-WorkBuddyAccessibility[\s\S]*?continue/);
  assert.match(target, /\[WorkBuddyUia\]::FromHandle\(\$handle\)/);
  assert.match(target, /MSAA应答/);
});

test("Windows WorkBuddy reads Chromium only through the native UIA COM client", () => {
  // The first managed AutomationElement in a process registers client-side
  // proxies that hide Chromium's native provider for every later request.
  const source = read("../src-tauri/src/codex_composer.rs");
  const between = (start, end) => source.split(start)[1].split(end)[0];
  const workbuddyPaths = [
    between("function Test-WorkBuddyElementOwner(", "function Get-ComposerCandidates("),
    between("function Find-WorkBuddyFocusedComposer(", "function Initialize-WorkBuddyAccessibility("),
    between("function Get-WorkBuddyTarget(", "function Get-CurrentVisibleTarget("),
    between("function Invoke-SendButton(", "} else {"),
  ];
  for (const path of workbuddyPaths) {
    assert.doesNotMatch(path, /Automation\.AutomationElement|Automation\.TreeWalker|Find-BoundedComposerElements|Find-PointComposerElements/);
  }
  assert.match(workbuddyPaths[3], /\$state.Agent -eq 'workbuddy'[\s\S]*\[WorkBuddyUia\]::FindBounded/);
  const native = between("public static class WorkBuddyUia {", '"@');
  assert.match(native, /e22ad333-b25f-460c-83d0-0581107395c9/);
  assert.match(native, /GetRawViewWalker\(\)/);
  const writable = between("function Test-WorkBuddyWritablePattern(", "function Test-WorkBuddyElementOwner(");
  assert.match(writable, /WorkBuddyUiaValuePattern/);
  assert.match(writable, /WorkBuddyUiaTextPattern/);
  assert.match(writable, /\$readOnly -is \[bool\] -and -not \$readOnly/);
});
