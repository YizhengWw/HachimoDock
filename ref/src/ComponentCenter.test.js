/**
 * [Input] ComponentCenter.jsx, App.jsx, and fixtures.js component-center source.
 * [Output] Static Node coverage for the rewritten hero + flat library + modal layout.
 *          Tests assert new NowShowingHero / CandidateCard / ComponentPreviewModal wiring,
 *          Step 3 auto-refresh guidance plus manual drag-add fallback, while verifying
 *          direct preview-modal OTA install, critical install pipelines, and cross-page contracts remain.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));

function readSource(fileName) {
  return readFileSync(join(srcDir, fileName), "utf8");
}

// ── Step 3: hero + library + modal layout ─────────────────────────────────────

test("ComponentCenter renders NowShowingHero at the top of the page", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /import\s+NowShowingHero/);
  assert.match(component, /<NowShowingHero/);
});

test("ComponentCenter renders the component library grid using CandidateCard", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /import\s+CandidateCard/);
  assert.match(component, /component-library-grid/);
  assert.match(component, /<CandidateCard/);
});

test("ComponentCenter opens ComponentPreviewModal on candidate click (transient selection)", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /import\s+ComponentPreviewModal/);
  assert.match(component, /previewComponent/);
  assert.match(component, /<ComponentPreviewModal/);
});

test("preview modal install button starts install directly instead of opening hidden replace confirm", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /currentComponent=\{currentFullComponent\}/);
  assert.match(component, /installSelectedComponent\(previewComponent\)/);
  assert.doesNotMatch(
    component,
    /onInstall=\{\(\) => \{[\s\S]*?setShowReplaceConfirm\(true\)[\s\S]*?\}\}/,
  );
});

test("USB preflight trusts the shared DeviceContext USB state", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /const deviceConnected = usb\.connected/);
  assert.match(component, /if \(!status\?\.connected && !deviceConnected\)/);
});

test("Library filters out the currently-installed component", () => {
  const component = readSource("ComponentCenter.jsx");
  // Should match a filter expression on currentComponent.id
  assert.match(component, /\.filter\([^)]*item[\s\S]*?currentComponent[\s\S]*?id/);
});

test("draft cards use manifest description instead of exposing local draft paths", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /draft\.description/);
  assert.match(component, /自定义草稿 · 可预览后安装到负一屏/);
  assert.doesNotMatch(component, /goal:\s*`自定义草稿 · \$\{draft\.path\.replace/);
});

test("Hero's onChangeRequest smooth-scrolls to the library section", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /component-library/);
  // Either scrollIntoView or a scrollTo to id "component-library"
  assert.match(component, /scrollIntoView|scrollTo/);
});

test("ComponentCenter keeps the 创建组件 actions button + drawer wiring", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /创建组件/);
  assert.match(component, /CreateComponentDrawer/);
  assert.match(component, /setCreateDrawerOpen/);
});

test("ComponentCenter writes pet-manager:active-component on successful install (driving ContextRail)", () => {
  const component = readSource("ComponentCenter.jsx");
  // Must remain from Plan 4 — critical cross-page contract.
  assert.match(component, /pet-manager:active-component/);
  assert.match(component, /new Event\("storage"\)/);
});

test("installing a widget no longer applies a red-button preset", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.doesNotMatch(component, /applyRecommendedButtonConfigForWidget/);
  assert.doesNotMatch(component, /已应用推荐按键/);
  assert.doesNotMatch(component, /顶部红钮交给当前组件/);
});

test("ComponentCenter uses useToast for success/error notifications", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /useToast\(/);
  assert.match(component, /push\(\s*\{[\s\S]*?tone:\s*"success"/);
  assert.match(component, /push\(\s*\{[\s\S]*?tone:\s*"error"/);
});

test("Existing install pipeline preserved (installBuiltinToDevice, installClawpkgFromPath)", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /installBuiltinToDevice/);
  assert.match(component, /installClawpkgFromPath/);
});

test("builtin component install resolves bundled resources instead of requiring ~/.openclaw", () => {
  const component = readSource("ComponentCenter.jsx");

  assert.match(component, /invoke\("resolve_builtin_clawpkg_path"/);
  assert.match(component, /const clawpkgPath = await invoke\("resolve_builtin_clawpkg_path"/);
  assert.doesNotMatch(component, /homeDir\(\)/);
  assert.doesNotMatch(component, /\.openclaw\/builtin-clawpkgs|\.openclaw\\builtin-clawpkgs/);
});

test("Tauri bundle includes built-in clawpkg resources", () => {
  const tauriConfig = JSON.parse(readFileSync(join(srcDir, "../src-tauri/tauri.conf.json"), "utf8"));
  assert.equal(
    tauriConfig.bundle.resources["../builtin-clawpkgs"],
    "builtin-clawpkgs",
  );
});

test("OTA modal, replace-confirm modal, and delete modal remain while prompt modal is removed", () => {
  const component = readSource("ComponentCenter.jsx");
  // The remaining modal sections handle USB OTA progress, replace confirm, and draft delete.
  // Prompt generation now launches the agent directly without showing a copy-prompt dialog.
  assert.match(component, /ota-modal|otaTargetName/);
  assert.match(component, /pendingDeleteDraft|deleteDraftDeleting/);
  assert.doesNotMatch(component, /aria-label="生成组件 prompt"/);
  assert.doesNotMatch(component, /component-generated-prompt/);
  assert.doesNotMatch(component, /当前渠道：/);
  assert.doesNotMatch(component, /MagicMirror 模块会先转换成 OpenClaw 组件/);
});

// ── Unchanged structural assertions ───────────────────────────────────────────

test("component center uses PageShell and shell components", () => {
  const component = readSource("ComponentCenter.jsx");
  const app = readSource("App.jsx");

  // Shell consumption
  assert.match(component, /from "\.\/shell\/PageShell\.jsx?"/);
  assert.match(component, /from "\.\/shell\/ToastStack\.jsx?"/);
  assert.match(component, /from "\.\/shell\/DeviceContext\.jsx?"/);
  assert.match(component, /<PageShell\b/);
  assert.match(component, /title="组件中心"/);
  assert.match(component, /useToast\(/);
  assert.match(component, /useDeviceContext\(/);

  // Action buttons in PageShell actions
  assert.match(component, /actions=\{/);
  assert.match(component, /刷新草稿/);
  assert.match(component, /setCreateDrawerOpen\(true\)/);

  // App.jsx wiring
  assert.match(app, /import ComponentCenter from "\.\/ComponentCenter"/);
  assert.match(app, /title="组件中心"/);
  assert.match(app, /<ComponentCenter \/>/);
});

test("component center preserves all install + delete pipelines", () => {
  const component = readSource("ComponentCenter.jsx");

  // localStorage cross-page contract
  assert.match(component, /localStorage\.setItem\(\s*ACTIVE_COMPONENT_STORAGE_KEY/);
  assert.match(component, /new Event\(\s*"storage"\s*\)/);

  // Toast notifications
  assert.match(component, /push\(\{[\s\S]*tone:\s*"success"/);
  assert.match(component, /push\(\{[\s\S]*tone:\s*"error"/);
  assert.doesNotMatch(component, /setClawpkgImportError\(/);
  assert.doesNotMatch(component, /setSkillInstallError\(/);
  assert.doesNotMatch(component, /component-tool-error/);

  // USB context (no local useState for usb)
  assert.doesNotMatch(component, /const \[usbConnected, setUsbConnected\] = useState/);
  assert.match(component, /usb\.connected/);

  // Core logic functions preserved
  assert.match(component, /function installSelectedComponent/);
  assert.match(component, /installBuiltinToDevice/);
  assert.match(component, /installClawpkgFromPath/);
  assert.match(component, /install_clawpkg_over_usb/);
  assert.match(component, /install_widget_skill/);
  assert.match(component, /delete_component_draft/);

  // Binding resolution preserved
  assert.match(component, /CONTROL_OPTIONS = \[/);
  assert.match(component, /屏幕点击/);
  assert.doesNotMatch(component, /顶部红钮短按/);
  assert.doesNotMatch(component, /label:\s*"旋钮旋转"/);
  assert.match(component, /const \[bindingOverrides, setBindingOverrides\] = useState\(\{\}\)/);
  assert.match(component, /buildBindingOverridesForInstall/);
  assert.match(component, /isRoutedWidgetBinding/);

  // Drafts pipeline preserved
  assert.match(component, /drafts\.map|drafts\.\(|drafts\.filter|list_component_drafts/);
  assert.match(component, /deleteDraftPath/);
  assert.match(component, /confirmDeleteDraft/);
  assert.match(component, /refreshDrafts/);

  // Modals preserved
  assert.match(component, /component-replace-modal/);
  assert.match(component, /替换负一屏确认|showReplaceConfirm/);
  assert.doesNotMatch(component, /component-generated-prompt/);
  assert.match(component, /ota-modal|otaTargetName/);
});

test("component center passes delete action to generated draft cards only", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /<CandidateCard[\s\S]*?onDelete=\{item\.isDraft \? \(\) => requestDeleteDraft\(item\) : undefined\}/);
  assert.match(component, /setDeleteDraftPath\(draft\.path \|\| draft\.draftPath\)/);
});

test("component center preserves component generation features", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /loadFollowedComponentGenerationAgentId/);
  assert.match(component, /labelForComponentGenerationAgent/);
  assert.match(component, /createSkillTriggerPrompt/);
  assert.match(component, /launch_agent_with_prompt/);
  assert.match(component, /生成组件启动失败/);
  assert.match(component, /const \[promptDraft, setPromptDraft\]/);
  assert.doesNotMatch(component, /createComponentGenerationCommand/);
  assert.doesNotMatch(component, /createAgentPrompt/);
});

test("component center matches draft component paths across Windows and POSIX separators", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /function pathContainsComponentId/);
  assert.match(component, /replaceAll\("\\\\", "\/"\)|replace\(\/\\\\\\\\\/g, "\/"\)/);
  assert.match(component, /function matchesDraftPath/);
  assert.match(component, /matchesDraftPath\(d, otaPendingPath\)/);
  assert.match(component, /matchesDraftPath\(d, clawpkgPath\)/);
  assert.doesNotMatch(component, /includes\(`\/\$\{d\.id\}`\)/);
});

test("tauri generation prompt launcher has a Windows terminal implementation", () => {
  const tauri = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
  assert.match(tauri, /async fn launch_agent_with_prompt/);
  assert.match(tauri, /#\[cfg\(target_os = "windows"\)\][\s\S]*run\.ps1/);
  assert.match(tauri, /#\[cfg\(target_os = "windows"\)\][\s\S]*powershell\.exe/);
  assert.match(tauri, /#\[cfg\(target_os = "windows"\)\][\s\S]*\.arg\("start"\)/);
  assert.doesNotMatch(tauri, /当前仅实现 macOS 终端启动/);
});

test("tauri SSH clawpkg install checks host ssh and tar commands up front", () => {
  const tauri = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
  assert.match(tauri, /fn require_host_command/);
  assert.match(tauri, /require_host_command\("tar"/);
  assert.match(tauri, /require_host_command\("ssh"/);
  assert.match(tauri, /Command::new\(&tar_bin\)/);
  assert.match(tauri, /Command::new\(&ssh_bin\)/);
});

test("petAgent widget skill keeps button clarification rules", () => {
  const widgetSkill = readFileSync(join(srcDir, "../../skills/petAgent-ui-generator/SKILL.md"), "utf8");
  assert.match(widgetSkill, /按钮配置追问规则/);
  assert.match(widgetSkill, /切到负一屏就是进入这个组件场景/);
  assert.match(widgetSkill, /screen\.region\.tap/);
  assert.match(widgetSkill, /screen\.region\.long_press/);
  assert.doesNotMatch(widgetSkill, /button\.primary\.short_press/);
  assert.match(widgetSkill, /旋钮旋转固定用于系统音量/);
  assert.doesNotMatch(widgetSkill, /knob\.rotate_cw \/ knob\.rotate_ccw/);
});

test("component center preserves CreateComponentDrawer with all 3 STEP cards", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /function CreateComponentDrawer\b/);
  assert.match(component, /component-center-drawer-backdrop/);
  assert.match(component, /component-center-drawer\b/);
  assert.match(component, /Escape/);
  assert.match(component, /STEP 1.*Skill/);
  assert.match(component, /STEP 2.*生成/);
  assert.match(component, /STEP 3.*自动更新/);
  assert.match(component, /component-tool-card--skill/);
  assert.match(component, /component-tool-card--generate/);
  assert.match(component, /component-tool-card--clawpkg/);
  assert.match(component, /component-clawpkg-dropzone/);
  assert.match(component, /handleClawpkgDrop/);
  assert.match(component, /handleInstallSkill/);
  assert.match(component, /skillInstallResult\.installed/);
  assert.match(component, /skillInstallResult\.skipped/);
  assert.match(component, /检测到的 Coding Agent|检测到的 coding agent/i);
});

test("create drawer explains generated components auto-refresh and exposes drag-add fallback button", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /STEP 3.*自动更新/);
  assert.match(component, /生成完成后组件中心会自动刷新并展示新草稿/);
  assert.match(component, /没看到更新/);
  assert.match(component, /拖拽或选择加入组件中心/);
  assert.match(component, /component-clawpkg-fallback-button/);
});

test("component center CSS has new library rules and no old layout rules", () => {
  const styles = readSource("styles.css");

  // New rules must exist
  assert.match(styles, /\.component-library-section\s*\{/);
  assert.match(styles, /\.component-library-grid\s*\{/);
  assert.match(styles, /\.component-center-drawer\s*\{/);
  assert.match(styles, /\.component-center-drawer-backdrop\s*\{/);

  // Old Plan 4 layout rules must be gone
  assert.doesNotMatch(styles, /\.component-center-grid-layout\s*\{/);
  assert.doesNotMatch(styles, /\.component-center-preview-aside\s*\{/);
  assert.doesNotMatch(styles, /\.component-center-preview-empty\s*\{/);

  // Still-in-use styles preserved
  assert.match(styles, /\.component-store-settings \.component-setting-card/);
  assert.match(styles, /\.component-store-card__actions/);
  assert.match(styles, /\.component-store-card__delete/);
});

test("library grid ends with a CreateNewCard placeholder tile", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /function CreateNewCard\s*\(/);
  assert.match(component, /candidate-card--create/);
  assert.match(component, /<CreateNewCard/);
  // CreateNewCard must appear after the libraryItems.map inside component-library-grid
  assert.match(component, /libraryItems\.map[\s\S]*?<CreateNewCard/);
});

test("CreateNewCard calls setCreateDrawerOpen when clicked", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /CreateNewCard[\s\S]*?onClick.*setCreateDrawerOpen\(true\)/);
});

test("fixtures preserved (no content regression)", () => {
  const data = readSource("fixtures.js");
  assert.match(data, /export const BUILTIN_COMPONENT_CENTER/);
  assert.match(data, /promptBuilder/);
  assert.match(data, /replacementPreview/);
  assert.match(data, /slack-off-countdown/);
  assert.match(data, /tomato-clock/);
  assert.match(data, /drink-reminder/);
  assert.match(data, /screen\.region\.tap/);
  assert.match(data, /screen\.region\.long_press/);
  assert.doesNotMatch(data, /button\.primary\.short_press/);
  assert.match(data, /固定用于系统音量调节/);
});

test("tauri backend delete wiring preserved", () => {
  const tauri = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
  assert.match(tauri, /async fn delete_component_draft/);
  assert.match(tauri, /component_drafts_root/);
  assert.match(tauri, /target\.starts_with\(&drafts_root\)/);
  assert.match(tauri, /delete_component_draft,/);
  assert.match(tauri, /"屏幕点击"\s*=>\s*\("屏幕区域",\s*"screen\.region\.tap"\)/);
});

test("tauri draft listing exposes component description for custom card summaries", () => {
  const tauri = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
  assert.match(tauri, /struct ComponentDraftEntry[\s\S]*description:\s*String/);
  assert.match(tauri, /fn read_component_description/);
  assert.match(tauri, /description:\s*read_component_description/);
});
