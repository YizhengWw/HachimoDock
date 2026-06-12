# Plan 4: Component Center Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `ref/src/ComponentCenter.jsx` so that the page consumes the shell from Plan 1: `<PageShell>` header with `刷新草稿` + `创建组件` actions (no chevron — drawer is the only sub-flow), two-column body (component-library grid + always-visible preview aside), the 3-step tutorial relocated into a right-side `CreateComponentDrawer`, and global `useToast()` notifications. Built-in and draft components stay in the same grid in the existing order (built-ins first, drafts after) with corner badges distinguishing source. Installing a component writes the active component to `localStorage["pet-manager:active-component"]` so the sidebar `ContextRail` (Plan 1) updates.

**Architecture:** All business logic — `installBuiltinToDevice`, `installClawpkgFromPath`, OTA modal state machine, draft management (`refreshDrafts` / `confirmDeleteDraft`), replace-confirm modal, prompt-builder modal, skill install, clawpkg drop — is preserved verbatim. Only the JSX layout, the toast surface, and the post-install side-effect are rewritten. A new local helper `CreateComponentDrawer` (declared inside `ComponentCenter.jsx`, NOT in `ref/src/shell/` — the shell is frozen by Plan 1) holds the 3 STEP cards behind a right-side slide-out drawer that closes on backdrop click or `Escape`. USB / online state moves to `useDeviceContext()` so the local USB-status poll currently driving the OTA modal can read shared state instead of polling on its own.

**Tech Stack:** React 18, lucide-react, vanilla CSS (existing `--accent`/`--line`/`--radius-*` tokens), `node:test` for static source analysis tests, Tauri `invoke` for backend calls (unchanged).

**Spec reference:** `docs/superpowers/specs/2026-05-26-pet-manager-layout-redesign-design.md` — section "Plan 4: Component Center" and its "关键决定" subsection.

**Plan 1 dependency:** This plan assumes Plan 1 is already merged. Shell API (PageShell / Card / useDeviceContext / useToast / ToastStack) and the `pet-manager:active-component` localStorage key are FROZEN — do not edit any file under `ref/src/shell/`.

---

## File Structure

**Modified files:**

| Path | What changes |
|------|--------------|
| `ref/src/ComponentCenter.jsx` | Body rewritten: `<PageShell>` + new two-column grid + `<CreateComponentDrawer>` (new local helper, defined in this same file at the bottom). All handlers/state preserved. Toast surface replaces all inline error / sync-notice strings. `installSelectedComponent` writes `pet-manager:active-component` after a successful install. The local USB-status `useState` + ad-hoc polling in `startOtaInstall` / OTA modal effect is replaced by reading `usb.connected` from `useDeviceContext()`. |
| `ref/src/ComponentCenter.test.js` | Adapted to assert the new structure: `<PageShell>` consumption, mixed grid with `data-source="builtin"`/`data-source="draft"` and corner badges preserved, `CreateComponentDrawer` opens on action click and closes on backdrop/ESC, preview soft-prompt when nothing selected, `localStorage` write on install, `useToast` calls replacing inline error strings. Existing behavioral assertions about CONTROL_OPTIONS / installBuiltinToDevice / install_clawpkg_over_usb / delete_component_draft / Step 1-3 copy are PORTED, not deleted (the 3 STEP cards still exist — they just live inside the drawer now). |
| `ref/src/styles.css` | Add a `/* === component-center (Plan 4) ===` section at the END of the file containing: `.component-center-grid-layout` (CSS grid main + aside), `.component-center-preview-aside`, `.component-center-preview-empty`, `.component-center-drawer-backdrop`, `.component-center-drawer`, `.component-center-drawer__head`. Existing classes (`.component-store-grid`, `.component-store-card`, `.builtin-card`, `.component-device-screen`, `.cds-*`, `.component-store-settings`, `.component-replace-modal`, `.ota-modal`, etc.) are KEPT and reused. |
| `ref/src/.folder.md` | If the existing `ComponentCenter.jsx` row's description references the inline 3-step layout, refresh that sentence to say "组件库网格 + 预览侧栏 + 创建组件抽屉，配合 Plan 1 shell"。 |

**Files explicitly NOT touched (out of scope for Plan 4):**

- Anything under `ref/src/shell/` (shell API frozen by Plan 1)
- `ref/src/App.jsx` (Plan 1 already wired providers; Plan 4 only consumes them)
- `ref/src/mock-data.js` (MOCK_COMPONENT_CENTER shape unchanged)
- `ref/src/lib/component-generation-template.js` (template helpers unchanged)
- `ref/src/lib/clawpkg-contract.js` (contract unchanged)
- `ref/src-tauri/**` (no Rust changes — same invoke commands)
- `ref/src/DeviceDashboard.jsx`, `ref/src/AppearanceGallery.jsx` (Plans 2/3 own them)

---

## Conventions used in this plan

- **Working directory** for shell commands is `/path/to/claw-pet-manager/ref` unless explicitly noted. Tests are invoked from that directory.
- **Test runner**: `node --test src/ComponentCenter.test.js`. The project has no `test` npm script — invoke `node --test` directly.
- **Test style**: static source analysis. Read `ComponentCenter.jsx` with `readFileSync`, assert with `node:assert/strict`. Same pattern as the existing `ComponentCenter.test.js` and Plan 1's shell tests. Do NOT introduce jest / React Testing Library.
- **Commit style**: conventional commits, Chinese subject OK. No `--no-verify`. Pattern: `feat(component-center): <one-line summary>` or `refactor(component-center): ...` / `test(component-center): ...`.
- **TDD cycle for every task**: write/extend test → run (FAIL) → implement → run (PASS) → commit. Each step is 2-5 minutes.
- **No new dependencies**: stay inside React + lucide-react + Tauri.
- **Naming locked**:
  - New helper component: `CreateComponentDrawer` (default-NOT-exported, declared inside `ComponentCenter.jsx`)
  - New state: `const [createDrawerOpen, setCreateDrawerOpen] = useState(false)`
  - New constant: `const ACTIVE_COMPONENT_STORAGE_KEY = "pet-manager:active-component"` (must match the key Plan 1's `DeviceContext.jsx` already reads)
  - Toast tones used: `success`, `error`, `info` (matches Plan 1's `ToastStack` ICONS map)

---

## Task 1: Adapt the static test for the new layout shape

Before touching the component, replace the layout-coupled assertions in `ref/src/ComponentCenter.test.js` so they describe the target shape. Behavioral assertions (about CONTROL_OPTIONS, installBuiltinToDevice, install_clawpkg_over_usb, MOCK_COMPONENT_CENTER content, button copy etc.) are preserved.

**Files:**
- Modify: `ref/src/ComponentCenter.test.js`

- [ ] **Step 1: Edit the test to remove obsolete layout assertions and add new ones**

Open `ref/src/ComponentCenter.test.js`. The existing single `test(...)` block contains many `assert.match(component, ...)` lines that are still valid (CONTROL_OPTIONS, MOCK consumption, button labels, install handlers, delete-draft modal, OTA modal). KEEP all of those. ADD the new assertions below and REMOVE only the ones that no longer apply once we move to PageShell + drawer.

Replace the existing test block with the following expanded version (this version is the COMPLETE replacement for the body of `test("component center stays app-like instead of becoming a setup document", () => { ... })`):

```javascript
test("component center stays app-like instead of becoming a setup document", () => {
  const data = readSource("mock-data.js");
  const component = readSource("ComponentCenter.jsx");
  const styles = readSource("styles.css");
  const app = readSource("App.jsx");
  const tauri = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
  const widgetSkill = readFileSync(join(srcDir, "../../skills/mipet-ui-generator/SKILL.md"), "utf8");

  // ---- mock-data (unchanged) ------------------------------------------------
  assert.match(data, /export const MOCK_COMPONENT_CENTER/);
  assert.match(data, /内置案例 \+ AI 生成组件/);
  assert.match(data, /promptBuilder/);
  assert.match(data, /没找到？直接描述组件需求/);
  assert.match(data, /componentGenerator/);
  assert.match(data, /Codex/);
  assert.match(data, /Claude Code/);
  assert.match(data, /MagicMirror/);
  assert.match(data, /MMM-/);
  assert.match(data, /MagicMirror 模块会先转换成 OpenClaw 组件/);
  assert.match(data, /replacementPreview/);
  assert.match(data, /当前负一屏已经安装/);
  assert.match(data, /component\.json/);
  assert.match(data, /negative-screen\.json/);
  assert.match(data, /buttons\.json/);
  assert.match(data, /runtime\//);
  assert.match(data, /share\.json/);
  assert.match(data, /slack-off-countdown/);
  assert.match(data, /摸鱼倒计时/);
  assert.match(data, /tomato-clock/);
  assert.match(data, /番茄钟/);
  assert.match(data, /drink-reminder/);
  assert.match(data, /喝水提醒/);
  assert.match(data, /progress:\s*\{\s*value:\s*30,\s*label:\s*"本次间隔"\s*\}/);
  assert.match(data, /clock\.switch_view/);
  assert.match(data, /timer\.start_pause/);
  assert.match(data, /reminder\.acknowledge/);
  assert.match(data, /dashboard/);
  assert.match(data, /button\.primary\.short_press/);
  assert.match(data, /button\.primary\.long_press/);
  assert.match(data, /knob\.rotate_cw \/ knob\.rotate_ccw/);
  assert.match(data, /点击 查看拆分 · 长按 刷新/);
  assert.match(data, /screen\.region\.tap/);
  assert.match(data, /screen\.region\.long_press/);
  assert.match(data, /packageIncludes/);
  assert.match(data, /hardwareControls/);
  assert.doesNotMatch(data, /世界时钟/);
  assert.doesNotMatch(data, /天气温度/);
  assert.doesNotMatch(data, /desktop\.weather/);

  // ---- Plan 4: shell consumption -------------------------------------------
  assert.match(component, /from "\.\/shell\/PageShell\.jsx?"/);
  assert.match(component, /from "\.\/shell\/Card\.jsx?"/);
  assert.match(component, /from "\.\/shell\/ToastStack\.jsx?"/);
  assert.match(component, /from "\.\/shell\/DeviceContext\.jsx?"/);
  assert.match(component, /<PageShell\b/);
  assert.match(component, /title="组件中心"/);
  assert.match(component, /subtitle="选一个负一屏组件，推到桌搭子"/);
  assert.match(component, /useToast\(/);
  assert.match(component, /useDeviceContext\(/);

  // ---- Plan 4: action buttons live in PageShell actions, not body ---------
  assert.match(component, /actions=\{/);
  assert.match(component, /刷新草稿/);                       // first action
  assert.match(component, /创建组件/);                       // second action opens drawer
  assert.match(component, /setCreateDrawerOpen\(true\)/);

  // ---- Plan 4: mixed grid (内置 first then drafts), badges preserved -------
  assert.match(component, /component-center-grid-layout/);
  assert.match(component, /MOCK_COMPONENT_CENTER\.components\.map/);
  assert.match(component, /drafts\.map/);
  assert.match(component, /component-store-card__badge--builtin/);
  assert.match(component, /component-store-card__badge--custom/);
  assert.match(component, /component-store-card--draft/);
  assert.match(component, /data-source="builtin"/);
  assert.match(component, /data-source="draft"/);

  // ---- Plan 4: preview aside always visible, soft prompt when nothing selected ----
  assert.match(component, /component-center-preview-aside/);
  assert.match(component, /component-center-preview-empty/);
  assert.match(component, /选一个组件预览和安装/);

  // ---- Plan 4: CreateComponentDrawer (right-side slide-out with backdrop + ESC) ----
  assert.match(component, /function CreateComponentDrawer\b/);
  assert.match(component, /component-center-drawer-backdrop/);
  assert.match(component, /component-center-drawer\b/);
  assert.match(component, /onClose\(\)/);
  // ESC handler — keydown listener inside drawer
  assert.match(component, /Escape/);

  // ---- Plan 4: drawer hosts the 3 STEP cards (existing copy preserved) ----
  assert.match(component, /STEP 1.*Skill/);
  assert.match(component, /STEP 2.*生成/);
  assert.match(component, /STEP 3.*安装/);
  assert.match(component, /component-tool-card--skill/);
  assert.match(component, /component-tool-card--generate/);
  assert.match(component, /component-tool-card--clawpkg/);

  // ---- Plan 4: localStorage write on install drives sidebar ContextRail ---
  assert.match(component, /pet-manager:active-component/);
  assert.match(component, /localStorage\.setItem\(\s*ACTIVE_COMPONENT_STORAGE_KEY/);
  assert.match(component, /new Event\(\s*"storage"\s*\)/);

  // ---- Plan 4: toasts replace inline error / sync-notice ------------------
  assert.match(component, /push\(\{[\s\S]*tone:\s*"success"/);
  assert.match(component, /push\(\{[\s\S]*tone:\s*"error"/);
  // Old inline error state vars must be GONE (they migrated to toast):
  assert.doesNotMatch(component, /setClawpkgImportError\(/);
  assert.doesNotMatch(component, /setSkillInstallError\(/);
  assert.doesNotMatch(component, /component-tool-error/);

  // ---- Plan 4: USB poll uses shared context, not local useState ----------
  // The local usbConnected state and its setter must be gone — replaced by useDeviceContext().usb
  assert.doesNotMatch(component, /const \[usbConnected, setUsbConnected\] = useState/);
  assert.match(component, /usb\.connected/);

  // ---- Behavioral logic (preserved from previous version) -----------------
  assert.match(component, /createComponentGenerationCommand/);
  assert.match(component, /loadFollowedComponentGenerationAgentId/);
  assert.match(component, /labelForComponentGenerationAgent/);
  assert.match(component, /选一个负一屏组件/);
  assert.match(component, /搜索或描述想要的组件/);
  assert.match(component, /生成组件/);
  assert.match(component, /当前渠道/);
  assert.match(component, /const magicMirrorHint = MOCK_COMPONENT_CENTER\.componentGenerator\.magicMirror/);
  assert.match(component, /magicMirrorHint\.detail/);
  assert.match(component, /组件库/);
  assert.match(component, /deleteDraftPath/);
  assert.match(component, /confirmDeleteDraft/);
  assert.match(component, /delete_component_draft/);
  assert.match(component, /删除组件/);
  assert.match(component, /component-store-card__actions/);
  assert.match(component, /component-store-card__delete/);
  assert.doesNotMatch(component, /component-drafts__list/);
  assert.doesNotMatch(component, /component-drafts__item/);
  assert.match(component, /安装预览/);
  assert.match(component, /替换负一屏确认/);
  assert.match(component, /确认替换/);
  assert.match(component, /按钮功能/);
  assert.match(component, /生成组件 prompt/);
  assert.match(component, /component-generated-prompt/);
  assert.match(component, /createAgentPrompt/);
  assert.match(component, /const \[promptDraft, setPromptDraft\]/);
  assert.match(component, /const \[selectedComponentId, setSelectedComponentId\]/);
  assert.match(component, /const \[activeNegativeScreenId, setActiveNegativeScreenId\]/);
  assert.match(component, /const \[showReplaceConfirm, setShowReplaceConfirm\]/);
  assert.match(component, /const \[generatedCommand, setGeneratedCommand\]/);
  assert.match(component, /CONTROL_OPTIONS = \[/);
  assert.match(component, /屏幕点击/);
  assert.match(component, /屏幕长按/);
  assert.match(component, /顶部红钮短按/);
  assert.match(component, /顶部红钮长按/);
  assert.match(component, /旋钮旋转/);
  assert.match(component, /const \[bindingOverrides, setBindingOverrides\] = useState\(\{\}\)/);
  assert.match(component, /buildBindingOverridesForInstall/);
  assert.match(component, /function installSelectedComponent/);
  assert.match(component, /function handleInstallClick/);
  assert.match(component, /function updateBinding/);
  assert.match(component, /function resetBindings/);
  assert.match(component, /点击 \/ 长按分别做什么/);
  assert.match(component, /没说清楚/);
  assert.match(component, /先追问/);
  assert.match(component, /component-store-grid/);
  assert.match(component, /component-store-card/);
  assert.match(component, /component-device-panel/);
  assert.match(component, /normalizeDashboardProgress/);
  assert.match(component, /cds-top-status/);
  assert.match(component, /cds-progress__meta/);
  assert.match(component, /bindingOverrides:\s*buildBindingOverridesForInstall/);
  assert.match(component, /component-device-bindings/);
  assert.match(component, /component-replace-modal/);
  assert.match(component, /component-store-settings/);
  assert.match(component, /control-choice-row/);
  assert.match(component, /install_clawpkg_over_usb/);
  assert.match(component, /安装到设备/);
  assert.match(component, /installBuiltinToDevice/);
  assert.match(component, /install_widget_skill/);
  assert.match(component, /handleInstallSkill/);
  assert.match(component, /一键安装 Skill/);
  assert.match(component, /skillInstallResult\.installed/);
  assert.match(component, /skillInstallResult\.skipped/);
  assert.match(component, /检测到的 Coding Agent|检测到的 coding agent/i);
  assert.match(component, /component-clawpkg-dropzone/);
  assert.match(component, /installClawpkgFromPath/);
  assert.match(component, /handleClawpkgDrop/);
  assert.doesNotMatch(component, /agent\.buttonLabel/);
  assert.doesNotMatch(component, /交给 Codex 生成/);
  assert.doesNotMatch(component, /交给 Claude Code 生成/);
  assert.doesNotMatch(component, /开发者说明/);
  assert.doesNotMatch(component, /component-package-file-grid/);
  assert.doesNotMatch(component, /component-upload-dropzone/);
  assert.doesNotMatch(component, /component-preview-cards/);
  assert.doesNotMatch(component, /component-install-steps/);
  assert.doesNotMatch(component, /component-replace-warning/);
  assert.doesNotMatch(component, /toggleInstalled/);
  assert.doesNotMatch(component, /上传什么/);
  assert.doesNotMatch(component, /搜索社区/);
  assert.doesNotMatch(component, /setActiveComponentId/);
  assert.doesNotMatch(component, /screenCount/);
  assert.doesNotMatch(component, /screen-count-control/);
  assert.doesNotMatch(component, /component-mapping-model/);
  assert.doesNotMatch(component, /event} -&gt;/);

  // ---- Plan 4: styles for new layout pieces -------------------------------
  assert.match(styles, /\.component-center-grid-layout\s*\{/);
  assert.match(styles, /\.component-center-preview-aside\s*\{/);
  assert.match(styles, /\.component-center-preview-empty\s*\{/);
  assert.match(styles, /\.component-center-drawer\s*\{/);
  assert.match(styles, /\.component-center-drawer-backdrop\s*\{/);

  // ---- Styles preserved from previous version (still in use) ---------------
  assert.match(styles, /\.component-store-settings \.component-setting-card/);
  assert.match(styles, /\.component-store-settings \.component-setting-card\s*\{[\s\S]*grid-template-columns:\s*1fr;/);
  assert.match(styles, /\.component-store-settings \.control-choice-row button\s*\{[\s\S]*white-space:\s*normal;/);
  assert.match(styles, /\.component-store-card__actions/);
  assert.match(styles, /\.component-store-card__delete/);

  // ---- App.jsx wiring (unchanged from Plan 1) -----------------------------
  assert.match(app, /import ComponentCenter from "\.\/ComponentCenter"/);
  assert.match(app, /title="组件中心"/);
  assert.match(app, /<ComponentCenter \/>/);

  // ---- Tauri backend (unchanged) ------------------------------------------
  assert.match(tauri, /async fn delete_component_draft/);
  assert.match(tauri, /component_drafts_root/);
  assert.match(tauri, /canonicalize_component_draft_path/);
  assert.match(tauri, /target\.starts_with\(&drafts_root\)/);
  assert.match(tauri, /remove_dir_all|remove_file/);
  assert.match(tauri, /delete_component_draft,/);

  assert.match(tauri, /"屏幕点击"\s*=>\s*\("屏幕区域",\s*"screen\.region\.tap"\)/);
  assert.match(tauri, /"顶部红钮短按"\s*=>\s*\("红色按钮",\s*"button\.primary\.short_press"\)/);
  assert.match(tauri, /"旋钮旋转"\s*=>\s*\("旋钮",\s*"knob\.rotate_cw \/ knob\.rotate_ccw"\)/);

  // ---- Widget skill contract (unchanged) ----------------------------------
  assert.match(widgetSkill, /按钮配置追问规则/);
  assert.match(widgetSkill, /screen\.region\.tap/);
  assert.match(widgetSkill, /screen\.region\.long_press/);
  assert.match(widgetSkill, /button\.primary\.short_press/);
  assert.match(widgetSkill, /knob\.rotate_cw/);
  assert.match(widgetSkill, /先追问/);
});
```

> The assertions are intentionally large so the test cleanly fails right now (before any source change). They will all pass after Tasks 2-7 finish.

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/ComponentCenter.test.js
```

Expected: FAIL. The failure should include several `AssertionError` lines for new patterns (`PageShell`, `component-center-grid-layout`, `CreateComponentDrawer`, `useDeviceContext`, `pet-manager:active-component`, ...) — those are the ones the next tasks will satisfy. It will also FAIL on `assert.doesNotMatch(component, /component-tool-error/)` etc., because the current source still has those.

- [ ] **Step 3: Commit the failing test**

```bash
cd /path/to/claw-pet-manager
git add ref/src/ComponentCenter.test.js
git commit -m "test(component-center): rewrite assertions for Plan 4 layout (RED)"
```

---

## Task 2: Wire imports, PageShell skeleton, toast + device context hooks

Get just enough of the shell wired so the test starts passing on the shell-consumption assertions and the actions are in place. Body is still mostly the old JSX — we strip and rebuild in Tasks 3-7.

**Files:**
- Modify: `ref/src/ComponentCenter.jsx`

- [ ] **Step 1: Replace imports at the top of `ComponentCenter.jsx`**

Open `ref/src/ComponentCenter.jsx`. Replace the existing import block (lines 8-29) with:

```jsx
import React, { useCallback, useEffect, useMemo, useState } from "react";
import {
  CheckCircle2,
  Clipboard,
  PackageCheck,
  RefreshCw,
  Search,
  RotateCcw,
  Settings2,
  Sparkles,
  Trash2,
  X,
} from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import { homeDir, join as pathJoin } from "@tauri-apps/api/path";
import { MOCK_COMPONENT_CENTER } from "./mock-data";
import {
  createAgentPrompt,
  createComponentGenerationCommand,
  createSkillTriggerPrompt,
  labelForComponentGenerationAgent,
  loadFollowedComponentGenerationAgentId,
} from "./lib/component-generation-template.js";
import PageShell from "./shell/PageShell.jsx";
import Card from "./shell/Card.jsx";
import { useToast } from "./shell/ToastStack.jsx";
import { useDeviceContext } from "./shell/DeviceContext.jsx";

const ACTIVE_COMPONENT_STORAGE_KEY = "pet-manager:active-component";
```

- [ ] **Step 2: Add toast + context + drawer state inside the component**

Find the function `export default function ComponentCenter() {` and the line declaring `const [selectedComponentId, setSelectedComponentId] = useState(...);` near the top of the body. Immediately BEFORE that `selectedComponentId` line, insert:

```jsx
  const { push: pushToast } = useToast();
  const { usb } = useDeviceContext();
  const [createDrawerOpen, setCreateDrawerOpen] = useState(false);
```

Then DELETE the existing local USB state line:

```jsx
  const [usbConnected, setUsbConnected] = useState(false);
```

and DELETE the existing error-tracking state lines (they migrate to toast):

```jsx
  const [skillInstallError, setSkillInstallError] = useState(null);
  const [clawpkgImportError, setClawpkgImportError] = useState(null);
```

(Keep `skillInstallResult` and `clawpkgImportResult` — those drive the UI body of the STEP 1 / STEP 3 cards and are still rendered inside the drawer.)

- [ ] **Step 3: Globally replace `usbConnected`/`setUsbConnected` references**

Anywhere in the file that still reads `usbConnected` or calls `setUsbConnected(...)`, swap as follows:

1. Replace expressions reading the local variable with `usb.connected`. Concrete locations:
   - `setUsbConnected(Boolean(status?.connected));` → DELETE the whole line (USB state is owned by `useDeviceContext` now)
   - `setUsbConnected(false);` → DELETE the whole line
   - `setUsbConnected(ok);` → DELETE the whole line
   - `usbConnected ? "USB 已连接,准备推送…" : "等待 USB 连接…"` → `usb.connected ? "USB 已连接,准备推送…" : "等待 USB 连接…"`
   - `<span className={`ota-modal__dot ${usbConnected ? "is-on" : "is-off"}`}` → `<span className={`ota-modal__dot ${usb.connected ? "is-on" : "is-off"}`}`

After this step, `grep -n usbConnected src/ComponentCenter.jsx` must return zero results except possibly comments. Run:

```bash
cd /path/to/claw-pet-manager/ref
grep -n "usbConnected\|setUsbConnected" src/ComponentCenter.jsx
```

Expected: empty output.

- [ ] **Step 4: Replace the top-level `return ( <div className="page page-component-center page-component-center--store"> ...` with a PageShell wrapper**

Find the line `return (\n    <div className="page page-component-center page-component-center--store">` (around the current line 599). Replace ONLY that opening `<div ...>` and the matching closing `</div>` (the last line before the function close `}`) with a `<PageShell>` skeleton. Inside the shell, place a temporary placeholder div that wraps EVERYTHING that used to be inside the page div (hero section, store-layout, OTA modal, replace modal, delete modal, prompt modal). Do not yet rewrite the body — Tasks 3-6 do that. The point of this step is to make the shell consumption assertions pass.

Concretely, the return becomes:

```jsx
  return (
    <PageShell
      title="组件中心"
      subtitle="选一个负一屏组件，推到桌搭子"
      actions={[
        <button
          key="refresh-drafts"
          type="button"
          className="btn-ghost btn-sm"
          onClick={refreshDrafts}
          disabled={draftsLoading}
        >
          <RefreshCw size={14} />
          {draftsLoading ? "扫描中…" : "刷新草稿"}
        </button>,
        <button
          key="open-create-drawer"
          type="button"
          className="btn-primary btn-sm"
          onClick={() => setCreateDrawerOpen(true)}
        >
          <Sparkles size={14} />
          创建组件
        </button>,
      ]}
    >
      <div className="page-component-center page-component-center--store">
        {/* TODO Plan 4 Task 3: replace this wrapper with .component-center-grid-layout */}
        <section className="component-store-hero">
          <div className="component-store-hero__copy">
            <span className="page-header__eyebrow">组件中心</span>
            <h1>选一个负一屏组件,推到桌搭子</h1>
            <p>从组件库里挑一个直接装,或用 coding agent skill 自己描述生成。</p>
          </div>
        </section>

        {/* === existing component-store-layout / OTA modal / replace modal / delete modal / prompt modal stay UNCHANGED in this step === */}
        {/* (Tasks 3-7 rebuild the body. Do not delete content here yet.) */}

        {/* paste the existing `<section className="component-store-layout"> ... </section>` block here, unchanged */}
        {/* paste the existing OTA modal block here, unchanged */}
        {/* paste the existing delete-draft modal block here, unchanged */}
        {/* paste the existing replace-confirm modal block here, unchanged */}
        {/* paste the existing generated-prompt modal block here, unchanged */}
      </div>
    </PageShell>
  );
```

Practically: cut the existing children of `<div className="page page-component-center page-component-center--store">` (everything between its open and close tags), drop the existing `<section className="component-store-hero">` (we just rebuilt it cleanly above — delete the duplicate that was at the top), and paste everything else (the `<section className="component-store-layout">`, the four modals) back inside the new placeholder `<div className="page-component-center page-component-center--store">` wrapper. The result is the SAME content, just wrapped in a `<PageShell>` with two action buttons. Body restructuring happens in Tasks 3-6.

- [ ] **Step 5: Run test to confirm shell-consumption assertions pass**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/ComponentCenter.test.js
```

Expected: still FAILS overall, but the failures should now be specifically about: `component-center-grid-layout`, `component-center-preview-aside`, `component-center-preview-empty`, `CreateComponentDrawer`, `pet-manager:active-component`, `localStorage.setItem`, `new Event("storage")`, `tone: "success"`, `tone: "error"`, `component-tool-error`, drawer CSS classes. The shell imports, `<PageShell ...>`, `useToast`, `useDeviceContext` should ALL pass.

- [ ] **Step 6: Smoke test in dev mode**

```bash
cd /path/to/claw-pet-manager/ref
npm run dev:web
```

Visit `http://localhost:4173`, click the sidebar "组件中心" tab. Verify:
- Page renders without console errors
- Page header shows "组件中心" / subtitle / "刷新草稿" + "创建组件" buttons in top-right (PageShell actions)
- Body still shows the old two-column layout with the 3 STEP cards inline (we haven't moved them yet)

Stop the dev server. If anything throws, fix the import path / hook usage before moving on.

- [ ] **Step 7: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/ComponentCenter.jsx
git commit -m "feat(component-center): wrap in PageShell + wire useToast/useDeviceContext"
```

---

## Task 3: Build the two-column grid layout (main grid + preview aside)

Replace the current `<section className="component-store-layout">` block with a new `<div className="component-center-grid-layout">` that has the same content split into a Card-wrapped main grid and a Card-wrapped preview aside. The grid rendering itself (built-ins + drafts + empty-hint) is preserved EXACTLY — we are only restructuring the wrappers.

**Files:**
- Modify: `ref/src/ComponentCenter.jsx`
- Modify: `ref/src/styles.css` (add `.component-center-grid-layout` block)

- [ ] **Step 1: Replace the `<section className="component-store-layout">` block**

In `ref/src/ComponentCenter.jsx`, find the line `<section className="component-store-layout">` and its matching `</section>`. Replace the ENTIRE block (from the `<section>` to the closing `</section>`, including the inner `<main className="component-store-main">` containing the section + the inline `<section className="component-center-tools">` STEP cards + the `<aside className="component-store-side">`) with:

```jsx
        <div className="component-center-grid-layout">
          {/* ── main: 组件库网格 ───────────────────────────── */}
          <Card
            title="组件库"
            subtitle={
              drafts.length > 0
                ? `内置组件 + 你用 AI agent 生成的草稿一起陈列 · 当前有 ${drafts.length} 个草稿`
                : "内置组件 + 你用 AI agent 生成的草稿一起陈列"
            }
          >
            <div className="component-store-grid">
              {MOCK_COMPONENT_CENTER.components.map((component) => {
                const isSelected = selectedComponent.id === component.id;
                const isOnDevice = activeNegativeScreenId === component.id;
                return (
                  <article
                    className={`builtin-card component-store-card${isSelected ? " is-selected" : ""}${isOnDevice ? " is-active" : ""}`}
                    data-widget={component.id}
                    data-source="builtin"
                    key={component.id}
                  >
                    <span className="component-store-card__badge component-store-card__badge--builtin">内置</span>
                    <button
                      type="button"
                      className="builtin-card__select"
                      onClick={() => setSelectedComponentId(component.id)}
                      aria-pressed={isSelected}
                    >
                      <header className="builtin-card__head">
                        <strong className="builtin-card__name">{component.name}</strong>
                        {isOnDevice ? (
                          <span className="builtin-card__pill builtin-card__pill--active">使用中</span>
                        ) : installedIds.has(component.id) ? (
                          <span className="builtin-card__pill">已安装</span>
                        ) : null}
                      </header>
                      <p className="builtin-card__desc">{component.goal}</p>
                      <span hidden data-action="安装到设备" data-handler="installBuiltinToDevice" />
                    </button>
                  </article>
                );
              })}
              {drafts.map((draft) => {
                const isSelected = selectedComponent.id === draft.id;
                const isOnDevice = activeNegativeScreenId === draft.id;
                const shortPath = draft.path.replace(/^.*\/component-drafts\//, "drafts/");
                return (
                  <article
                    className={`builtin-card component-store-card component-store-card--draft${isSelected ? " is-selected" : ""}${isOnDevice ? " is-active" : ""}`}
                    data-widget={draft.id}
                    data-source="draft"
                    key={draft.path}
                  >
                    <span className="component-store-card__badge component-store-card__badge--custom">自定义</span>
                    <button
                      type="button"
                      className="builtin-card__select"
                      onClick={() => setSelectedComponentId(draft.id)}
                      aria-pressed={isSelected}
                    >
                      <header className="builtin-card__head">
                        <strong className="builtin-card__name">{draft.name || draft.id}</strong>
                        {isOnDevice && (
                          <span className="builtin-card__pill builtin-card__pill--active">使用中</span>
                        )}
                      </header>
                      <p className="builtin-card__desc">
                        <code title={draft.path}>{shortPath}</code>
                      </p>
                    </button>
                    <div className="component-store-card__actions">
                      <button
                        type="button"
                        className="btn-primary btn-sm component-store-card__install"
                        onClick={() => installClawpkgFromPath(draft.path, { targetName: draft.name, skipFooterOverride: true })}
                        disabled={clawpkgImporting || deleteDraftDeleting}
                      >
                        安装到设备
                      </button>
                      <button
                        type="button"
                        className="btn-ghost danger btn-sm component-store-card__delete"
                        onClick={() => requestDeleteDraft(draft)}
                        disabled={clawpkgImporting || deleteDraftDeleting}
                        aria-label={`删除组件 ${draft.name || draft.id}`}
                      >
                        <Trash2 size={13} />
                        删除
                      </button>
                    </div>
                  </article>
                );
              })}
              {drafts.length === 0 && (
                <article className="builtin-card component-store-card component-store-card--empty-hint">
                  <div className="builtin-card__select">
                    <header className="builtin-card__head">
                      <strong className="builtin-card__name">还没有草稿</strong>
                    </header>
                    <p className="builtin-card__desc">
                      点右上 <strong>创建组件</strong> 装 skill / 描述生成 / 拖入 clawpkg。生成的草稿会自动出现在这里。
                    </p>
                  </div>
                </article>
              )}
            </div>
          </Card>

          {/* ── aside: 安装预览 (always visible) ─────────────── */}
          <aside className="component-center-preview-aside">
            {/* Filled in by Task 4 */}
          </aside>
        </div>
```

Also DELETE the inline `<section className="component-center-tools"> ... </section>` block (the 3 STEP cards) that you just removed from `<main className="component-store-main">`. Those cards re-appear inside the drawer in Task 5 — do not paste them back into the body.

- [ ] **Step 2: Add CSS for the grid layout**

In `ref/src/styles.css`, scroll to the end of the file and append a new section:

```css
/* === component-center (Plan 4) ======================================= */

.component-center-grid-layout {
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: 14px;
}

@media (min-width: 1100px) {
  .component-center-grid-layout {
    grid-template-columns: minmax(0, 1fr) minmax(300px, clamp(320px, 26vw, 360px));
    align-items: start;
  }
}

.component-center-preview-aside {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.component-center-preview-empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  padding: 32px 16px;
  color: var(--text-muted);
  text-align: center;
  font-size: 13px;
  line-height: 1.5;
}

.component-center-preview-empty svg { opacity: 0.5; }
```

- [ ] **Step 3: Run test**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/ComponentCenter.test.js
```

Expected: passes for `.component-center-grid-layout`, MOCK + drafts maps, builtin/draft badges. Still fails on preview-aside content, drawer, localStorage write, toast tones — those are upcoming tasks.

- [ ] **Step 4: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/ComponentCenter.jsx ref/src/styles.css
git commit -m "feat(component-center): two-column grid layout (main + preview aside)"
```

---

## Task 4: Build the preview aside (always-visible Card with soft-prompt fallback)

The aside must render the device-screen preview, the binding rows, the per-action customization details, and the install button — same content as the old `<section className="component-device-panel">`. When no component is selected (defensive — happens for the brief moment between deleting a draft and the next render), show a centered soft prompt.

**Files:**
- Modify: `ref/src/ComponentCenter.jsx`

- [ ] **Step 1: Fill the `<aside className="component-center-preview-aside">` from Task 3**

Find the `<aside className="component-center-preview-aside">` element you scaffolded in Task 3. Replace its `{/* Filled in by Task 4 */}` placeholder with:

```jsx
            {selectedComponent ? (
              <Card
                title="安装预览"
                subtitle={`${selectedComponent.name} 会替换当前负一屏显示。`}
              >
                <div className="component-device-preview">
                  <div
                    className="component-device-screen"
                    data-widget={selectedComponent.id}
                    aria-label={`${selectedComponent.name} 设备屏预览`}
                  >
                    {(() => {
                      const dashboard = selectedComponent.dashboard || {};
                      const progress = normalizeDashboardProgress(dashboard.progress);
                      return (
                        <>
                          <div className="cds-row-top">
                            {dashboard.title && <div className="cds-title-badge">{dashboard.title}</div>}
                            <div className="cds-top-status">
                              {dashboard.headline && <div className="cds-headline">{dashboard.headline}</div>}
                              {dashboard.badge && <div className="cds-badge-circle">{dashboard.badge}</div>}
                            </div>
                          </div>
                          {dashboard.eyebrow && <div className="cds-eyebrow">{dashboard.eyebrow}</div>}
                          {(dashboard.metricLabel || dashboard.metricValue) && (
                            <div className="cds-metric-panel">
                              {dashboard.metricLabel && <div className="cds-metric-label">{dashboard.metricLabel}</div>}
                              <div className="cds-metric-row">
                                {dashboard.metricValue && <span className="cds-metric-value">{dashboard.metricValue}</span>}
                                {dashboard.metricUnit && <span className="cds-metric-unit">{dashboard.metricUnit}</span>}
                              </div>
                              {dashboard.note && <div className="cds-note">{dashboard.note}</div>}
                              {progress && (
                                <div className="cds-progress" aria-label={progress.label || "进度"}>
                                  <div className="cds-progress__meta">
                                    <span>{progress.label || "进度"}</span>
                                    <span>{Math.round(progress.value)}%</span>
                                  </div>
                                  <div className="cds-progress__bar"><span style={{ width: `${progress.value}%` }} /></div>
                                </div>
                              )}
                            </div>
                          )}
                        </>
                      );
                    })()}
                  </div>
                </div>

                <div className="component-device-bindings">
                  {primaryBindings.map((binding) => {
                    const option = resolveControlOption(binding);
                    return (
                      <article key={binding.action}>
                        <span>{option.label}</span>
                        <strong>{binding.label}</strong>
                      </article>
                    );
                  })}
                </div>

                <details className="component-store-settings">
                  <summary>
                    <Settings2 size={15} />
                    按钮功能（随安装下发）
                  </summary>
                  <div className="component-store-settings__body">
                    <button className="btn-ghost btn-sm" type="button" onClick={resetBindings}>
                      <RotateCcw size={14} />
                      恢复默认
                    </button>
                    {selectedComponent.defaultBindings.length === 0 && (
                      <p className="component-store-settings__empty">
                        这个自定义组件会使用包内 buttons.json；需要改按钮功能时，请在生成草稿时说明。
                      </p>
                    )}
                    {selectedComponent.defaultBindings.map((binding) => {
                      const currentOption = resolveControlOption(binding);
                      return (
                        <article className="component-setting-card" key={binding.action}>
                          <div>
                            <span>{binding.label}</span>
                            <strong>{currentOption.label}</strong>
                            <p>{currentOption.help}</p>
                            <small>{currentOption.event}</small>
                          </div>
                          <div className="control-choice-row" aria-label={`${binding.label} 的按钮功能`}>
                            {CONTROL_OPTIONS.map((control) => (
                              <button
                                className={currentOption.label === control.label ? "is-selected" : ""}
                                key={control.label}
                                type="button"
                                onClick={() => updateBinding(binding, control.label)}
                                title={control.event}
                              >
                                {control.label}
                              </button>
                            ))}
                          </div>
                        </article>
                      );
                    })}
                  </div>
                </details>

                <button
                  className={isActiveOnDevice ? "btn-secondary" : "btn-primary"}
                  type="button"
                  disabled={isActiveOnDevice}
                  onClick={handleInstallClick}
                >
                  {isActiveOnDevice ? <CheckCircle2 size={15} /> : <PackageCheck size={15} />}
                  {isActiveOnDevice ? "已是设备当前负一屏" : `推到设备 · ${selectedComponent.name}`}
                </button>
              </Card>
            ) : (
              <Card title="安装预览">
                <div className="component-center-preview-empty">
                  <Search size={28} />
                  <p>选一个组件预览和安装</p>
                </div>
              </Card>
            )}
```

> The "no-selection" branch is technically defensive — `selectedComponent` falls back to `MOCK_COMPONENT_CENTER.components[0]` today, so it's almost always non-null. We keep the empty branch for the brief window after deleting the currently-selected draft (before `setSelectedComponentId` runs) AND to satisfy the spec's "未选时显示软提示". The branch ALSO makes the `选一个组件预览和安装` copy visible in the JSX source for the test grep.

To guarantee the empty branch is reachable, change the `selectedComponent` memo so it can yield `null` when nothing matches. Find:

```jsx
  const selectedComponent = useMemo(() => {
    const builtin = MOCK_COMPONENT_CENTER.components.find((c) => c.id === selectedComponentId);
    if (builtin) return builtin;
    const draft = drafts.find((d) => d.id === selectedComponentId);
    if (draft) return buildDraftAsComponent(draft);
    return MOCK_COMPONENT_CENTER.components[0];
  }, [selectedComponentId, drafts]);
```

Replace its final fallback line `return MOCK_COMPONENT_CENTER.components[0];` with:

```jsx
    return null;
  }, [selectedComponentId, drafts]);
```

Then update `selectedComponent` consumers that previously relied on the fallback being non-null:

1. `const isActiveOnDevice = activeNegativeScreenId === selectedComponent.id;` → `const isActiveOnDevice = !!selectedComponent && activeNegativeScreenId === selectedComponent.id;`
2. `const primaryBindings = selectedComponent.defaultBindings.slice(0, 3);` → `const primaryBindings = selectedComponent?.defaultBindings?.slice(0, 3) ?? [];`
3. In `installSelectedComponent`, wrap the body with `if (!selectedComponent) return;` at the top.
4. In `handleInstallClick`, wrap with `if (!selectedComponent) return;` at the top.
5. In `installClawpkgFromPath`, change `const guessedId = draftMatch?.id || builtinMatch?.id || selectedComponent.id;` → `const guessedId = draftMatch?.id || builtinMatch?.id || selectedComponent?.id || "";`

Initialize `selectedComponentId` so the page has a default selection on first paint (preserves today's behavior):

```jsx
  const [selectedComponentId, setSelectedComponentId] = useState(MOCK_COMPONENT_CENTER.components[0]?.id || "");
```

(That line is already there — leave it.)

Finally, DELETE the now-orphaned legacy aside that was kept around during Task 3. Specifically: any leftover `<aside className="component-store-side"> ... </aside>` and any leftover top-level `<section className="component-store-hero">` (we already render the title via `<PageShell title=...>`). After this step, the only top-level content under `<div className="page-component-center page-component-center--store">` should be: the `.component-center-grid-layout` div + the four modals (OTA / delete / replace / generated-prompt). Verify with:

```bash
cd /path/to/claw-pet-manager/ref
grep -n "component-store-side\|component-store-hero\|component-store-layout\|component-store-main\|component-center-tools" src/ComponentCenter.jsx
```

Expected: empty output for `component-store-side`, `component-store-layout`, `component-store-main`, `component-center-tools`. (`component-store-hero` may still appear in a comment — that's fine.)

- [ ] **Step 2: Run test**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/ComponentCenter.test.js
```

Expected: passes on `component-center-preview-aside`, `component-center-preview-empty`, `选一个组件预览和安装`, `component-device-screen`, `component-device-bindings`. Still fails on drawer/localStorage/toast assertions.

- [ ] **Step 3: Smoke test**

```bash
cd /path/to/claw-pet-manager/ref
npm run dev:web
```

Verify the page now has a two-column layout: grid on the left, preview Card on the right. Click any component card — preview updates. Stop dev server.

- [ ] **Step 4: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/ComponentCenter.jsx
git commit -m "feat(component-center): always-visible preview aside with soft prompt fallback"
```

---

## Task 5: Implement the `CreateComponentDrawer` helper

A right-side slide-out drawer that hosts the 3 STEP cards. Closes on backdrop click or Escape. The 3 cards' content is verbatim from the current `<section className="component-center-tools">` block (which Task 3 deleted from the body); we lift it into a new local helper component at the bottom of the file.

**Files:**
- Modify: `ref/src/ComponentCenter.jsx`
- Modify: `ref/src/styles.css` (add `.component-center-drawer*` rules)

- [ ] **Step 1: Add the drawer trigger right after the modals**

In `ref/src/ComponentCenter.jsx`, scroll to the four existing modals at the bottom of `<div className="page-component-center page-component-center--store">`. Immediately AFTER the last modal (the `{generatedCommand && (...)}` block) and BEFORE the closing `</div>` + `</PageShell>`, insert:

```jsx
        {createDrawerOpen && (
          <CreateComponentDrawer
            onClose={() => setCreateDrawerOpen(false)}
            followedAgentId={followedAgentId}
            followedAgentLabel={followedAgentLabel}
            promptDraft={promptDraft}
            setPromptDraft={setPromptDraft}
            handleGenerateClick={handleGenerateClick}
            handleInstallSkill={handleInstallSkill}
            skillInstalling={skillInstalling}
            skillInstallResult={skillInstallResult}
            clawpkgDragOver={clawpkgDragOver}
            setClawpkgDragOver={setClawpkgDragOver}
            handleClawpkgDrop={handleClawpkgDrop}
            handleClawpkgFilePick={handleClawpkgFilePick}
            clawpkgImporting={clawpkgImporting}
            clawpkgImportResult={clawpkgImportResult}
          />
        )}
```

- [ ] **Step 2: Declare `CreateComponentDrawer` at the bottom of the file**

After the closing `}` of `export default function ComponentCenter()`, append:

```jsx
function CreateComponentDrawer({
  onClose,
  followedAgentId,
  followedAgentLabel,
  promptDraft,
  setPromptDraft,
  handleGenerateClick,
  handleInstallSkill,
  skillInstalling,
  skillInstallResult,
  clawpkgDragOver,
  setClawpkgDragOver,
  handleClawpkgDrop,
  handleClawpkgFilePick,
  clawpkgImporting,
  clawpkgImportResult,
}) {
  useEffect(() => {
    function onKey(event) {
      if (event.key === "Escape") onClose();
    }
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);

  return (
    <div
      className="component-center-drawer-backdrop"
      role="presentation"
      onClick={onClose}
    >
      <aside
        className="component-center-drawer"
        role="dialog"
        aria-modal="true"
        aria-label="创建组件"
        onClick={(event) => event.stopPropagation()}
      >
        <header className="component-center-drawer__head">
          <div>
            <h2>创建组件</h2>
            <p>3 步走：装 skill → 用自然语言生成 .clawpkg → 拖回这里推到设备。</p>
          </div>
          <button
            type="button"
            className="icon-btn"
            onClick={onClose}
            aria-label="关闭抽屉"
          >
            <X size={16} />
          </button>
        </header>

        <article className="component-tool-card component-tool-card--skill">
          <header>
            <span className="component-tool-eyebrow">STEP 1 · 装 Skill</span>
            <h3>把 mipet-ui-generator 装到检测到的 Coding Agent</h3>
            <p>自动扫描 <code>~/.claude/</code> · <code>~/.codex/</code> · <code>~/.openclaw/</code> · <code>~/.gemini/</code> · <code>~/.cursor/</code>,把 skill 装到每个检测到的 agent。装好后任一会话里说"做个桌搭子组件"自动触发。</p>
          </header>
          <button
            className="btn-primary component-skill-install-button"
            type="button"
            onClick={handleInstallSkill}
            disabled={skillInstalling}
          >
            <PackageCheck size={15} />
            {skillInstalling ? "正在安装…" : "一键安装 Skill"}
          </button>
          {skillInstallResult && (
            <div className="component-skill-install-result">
              {skillInstallResult.installed.length > 0 && (
                <>
                  <p className="component-tool-result__title">已安装到 {skillInstallResult.installed.length} 个 coding agent</p>
                  <ul>
                    {skillInstallResult.installed.map((entry) => (
                      <li key={entry.agent}>
                        <strong>{entry.agent}</strong>
                        <span>{entry.fileCount} 文件{entry.overwrote ? " · 覆盖更新" : ""}</span>
                        <code>{entry.targetPath}</code>
                      </li>
                    ))}
                  </ul>
                </>
              )}
              {skillInstallResult.skipped.length > 0 && (
                <details className="component-skill-install-skipped">
                  <summary>跳过了 {skillInstallResult.skipped.length} 个未检测到的 agent</summary>
                  <ul>
                    {skillInstallResult.skipped.map((entry) => (
                      <li key={entry.agent}>
                        <strong>{entry.agent}</strong>
                        <span>{entry.reason}</span>
                      </li>
                    ))}
                  </ul>
                </details>
              )}
            </div>
          )}
        </article>

        <article className="component-tool-card component-tool-card--generate">
          <header>
            <span className="component-tool-eyebrow">STEP 2 · 描述生成</span>
            <h3>用文字描述,直接生成</h3>
            <p>装好 Skill 后,点按钮会调起当前跟随的 <code>{followedAgentLabel}</code> terminal,把你的描述喂进去,skill 自动接管生成 <code>.clawpkg</code>。尽量把点击 / 长按分别做什么说清楚，也可以说明红钮或旋钮要绑定的功能；没说清楚时 skill 会先追问。</p>
          </header>
          <textarea
            className="component-generate-textarea"
            aria-label="搜索或描述想要的组件"
            value={promptDraft}
            onChange={(event) => setPromptDraft(event.target.value)}
            placeholder={MOCK_COMPONENT_CENTER.promptBuilder.placeholder}
          />
          <p className="component-generate-guidance">
            可写:组件用途、显示哪些数字/状态、点击 screen.region.tap 做什么、长按 screen.region.long_press 做什么，也可以指定 button.primary 或 knob.rotate 的功能。
          </p>
          <button className="btn-primary" type="button" onClick={handleGenerateClick}>
            <Sparkles size={15} />
            生成组件
          </button>
        </article>

        <article className="component-tool-card component-tool-card--clawpkg">
          <header>
            <span className="component-tool-eyebrow">STEP 3 · 拖回安装</span>
            <h3>把外部 .clawpkg 推到设备</h3>
            <p>agent 生成的草稿会自动出现在左侧的组件库里。这里用来装从外部拿到的 <code>.clawpkg</code> 目录或 zip。</p>
          </header>
          <div
            className={`component-clawpkg-dropzone ${clawpkgDragOver ? "is-dragover" : ""}`}
            onDragOver={(e) => { e.preventDefault(); setClawpkgDragOver(true); }}
            onDragLeave={() => setClawpkgDragOver(false)}
            onDrop={handleClawpkgDrop}
          >
            <Clipboard size={20} />
            <span>{clawpkgImporting ? "正在校验 + 推送到设备…" : "或拖入 .clawpkg 目录 / zip"}</span>
          </div>
          <button
            type="button"
            className="btn-secondary component-clawpkg-pick-button"
            onClick={handleClawpkgFilePick}
            disabled={clawpkgImporting}
          >
            选择 .clawpkg 文件
          </button>
          {clawpkgImportResult && (
            <p className="component-tool-result__inline">
              已安装: <strong>{clawpkgImportResult.manifest.name}</strong>（{clawpkgImportResult.transferredBytes} bytes）
            </p>
          )}
        </article>
      </aside>
    </div>
  );
}
```

> Note we DROPPED `{skillInstallError && <p className="component-tool-error">...</p>}` and `{clawpkgImportError && <p className="component-tool-error">...</p>}` — those errors are surfaced via toast in Task 6.

- [ ] **Step 3: Add drawer CSS**

In `ref/src/styles.css`, append to the `/* === component-center (Plan 4) ===` section you started in Task 3:

```css
.component-center-drawer-backdrop {
  position: fixed;
  inset: 0;
  background: rgba(15, 23, 42, 0.45);
  display: flex;
  justify-content: flex-end;
  z-index: 1200;
  animation: component-center-drawer-fade-in 160ms ease;
}

.component-center-drawer {
  width: min(440px, 92vw);
  height: 100%;
  background: var(--surface);
  border-left: 1px solid var(--line);
  box-shadow: var(--shadow-lg, 0 12px 32px rgba(15, 23, 42, 0.18));
  display: flex;
  flex-direction: column;
  gap: 16px;
  padding: 20px;
  overflow-y: auto;
  animation: component-center-drawer-slide 200ms ease;
}

.component-center-drawer__head {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 12px;
  padding-bottom: 12px;
  border-bottom: 1px solid var(--line-soft);
}

.component-center-drawer__head h2 {
  margin: 0 0 4px;
  font-size: 15px;
  font-weight: 600;
}

.component-center-drawer__head p {
  margin: 0;
  font-size: 12px;
  color: var(--text-muted);
  line-height: 1.5;
}

@keyframes component-center-drawer-fade-in {
  from { opacity: 0; }
  to { opacity: 1; }
}

@keyframes component-center-drawer-slide {
  from { transform: translateX(100%); }
  to { transform: translateX(0); }
}
```

- [ ] **Step 4: Run test**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/ComponentCenter.test.js
```

Expected: passes on `function CreateComponentDrawer`, drawer classes, Escape, all 3 STEP card classes. Still fails on localStorage write + toast tones (Task 6).

- [ ] **Step 5: Smoke test**

```bash
cd /path/to/claw-pet-manager/ref
npm run dev:web
```

Click "创建组件" in the page header — drawer slides in from the right. Click outside (backdrop) — drawer closes. Press Escape — drawer closes. All 3 STEP cards render inside; STEP 1 install button still works (try it). Stop dev server.

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/ComponentCenter.jsx ref/src/styles.css
git commit -m "feat(component-center): CreateComponentDrawer with 3 STEP cards"
```

---

## Task 6: Migrate inline error displays to `useToast()` and add post-install localStorage write

This task does two things at once — both are short, both touch the same handlers, and the test asserts on both. Doing them in the same commit keeps the file consistent.

**Files:**
- Modify: `ref/src/ComponentCenter.jsx`

- [ ] **Step 1: Make `installSelectedComponent` write the active component**

Find `async function installSelectedComponent() {`. Add the localStorage write + storage event dispatch right after `setActiveNegativeScreenId(selectedComponent.id);` and BEFORE the install dispatch. The full updated function:

```jsx
  async function installSelectedComponent() {
    if (!selectedComponent) return;
    setInstalledIds((current) => {
      const next = new Set(current);
      next.add(selectedComponent.id);
      return next;
    });
    setActiveNegativeScreenId(selectedComponent.id);
    setShowReplaceConfirm(false);

    // Plan 4: write the active component so the sidebar ContextRail (Plan 1) updates.
    // DeviceContext.jsx already listens to the "storage" event and re-reads this key.
    try {
      localStorage.setItem(
        ACTIVE_COMPONENT_STORAGE_KEY,
        JSON.stringify({ id: selectedComponent.id, name: selectedComponent.name }),
      );
      window.dispatchEvent(new Event("storage"));
    } catch (err) {
      console.warn("[ComponentCenter] failed to persist active component", err);
    }

    if (selectedComponent.draftPath) {
      await installClawpkgFromPath(selectedComponent.draftPath, {
        targetName: selectedComponent.name,
        skipFooterOverride: true,
      });
    } else {
      await installBuiltinToDevice(selectedComponent.id);
    }
  }
```

- [ ] **Step 2: Replace skill-install error state with toast**

Find `async function handleInstallSkill()`. Replace the body so it surfaces both success and error via toast, AND keeps `skillInstallResult` (still used by the drawer to render the installed/skipped agent lists):

```jsx
  async function handleInstallSkill() {
    setSkillInstalling(true);
    try {
      const result = await invoke("install_widget_skill");
      setSkillInstallResult(result);
      const installedCount = result?.installed?.length ?? 0;
      pushToast({
        tone: installedCount > 0 ? "success" : "info",
        title: installedCount > 0
          ? `Skill 已安装到 ${installedCount} 个 coding agent`
          : "未检测到可安装的 coding agent",
      });
    } catch (err) {
      const msg = typeof err === "string" ? err : String(err);
      pushToast({
        tone: "error",
        title: "Skill 安装失败",
        message: msg,
      });
    } finally {
      setSkillInstalling(false);
    }
  }
```

- [ ] **Step 3: Replace clawpkg import error state with toast**

Find `async function installClawpkgFromPath(clawpkgPath, options = {}) {`. Replace the body:

```jsx
  async function installClawpkgFromPath(clawpkgPath, options = {}) {
    setClawpkgImporting(true);
    try {
      const draftMatch = drafts.find((d) => d.path === clawpkgPath || clawpkgPath.includes(`/${d.id}`));
      const builtinMatch = MOCK_COMPONENT_CENTER.components
        .find((c) => clawpkgPath.includes(c.id));
      const guessedId = draftMatch?.id || builtinMatch?.id || selectedComponent?.id || "";
      const resolvedOptions = draftMatch
        ? { targetName: draftMatch.name, skipFooterOverride: true, ...options }
        : options;
      await startOtaInstall(guessedId, clawpkgPath, resolvedOptions);
    } catch (err) {
      const msg = typeof err === "string" ? err : String(err);
      pushToast({
        tone: "error",
        title: "安装 .clawpkg 失败",
        message: msg,
      });
    } finally {
      setClawpkgImporting(false);
    }
  }
```

Also update `handleClawpkgDrop` to use toast for the file-path errors:

```jsx
  async function handleClawpkgDrop(event) {
    event.preventDefault();
    setClawpkgDragOver(false);
    const file = event.dataTransfer.files && event.dataTransfer.files[0];
    if (!file) {
      pushToast({ tone: "error", title: "没有读到文件" });
      return;
    }
    const localPath = file.path || file.webkitRelativePath;
    if (!localPath) {
      pushToast({
        tone: "error",
        title: "无法获取本地路径",
        message: "浏览器模式下拖拽不支持获取真实路径,请用 Tauri 桌面模式或'选择文件'按钮。",
      });
      return;
    }
    await installClawpkgFromPath(localPath);
  }
```

Update `handleClawpkgFilePick` the same way:

```jsx
  async function handleClawpkgFilePick() {
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".clawpkg,.zip";
    input.onchange = (e) => {
      const file = e.target.files && e.target.files[0];
      if (file && file.path) {
        installClawpkgFromPath(file.path);
      } else {
        pushToast({
          tone: "error",
          title: "无法获取本地路径",
          message: "浏览器模式下没有本地路径访问。请用桌面应用模式。",
        });
      }
    };
    input.click();
  }
```

- [ ] **Step 4: Surface OTA success/failure via toast (in addition to the existing modal)**

The OTA modal stays — it is the primary success/error surface during install. We additionally fire a toast on completion so the user sees confirmation even if they've already dismissed the modal. Edit `performOtaInstall` to push toasts. Find the success-and-error branches at the bottom of `performOtaInstall` and update:

```jsx
      if (!result.ok) {
        setOtaError(`校验失败: ${result.errors.join("; ")}`);
        setOtaPhase("error");
        pushToast({ tone: "error", title: "安装失败", message: result.errors.join("; ") });
        return;
      }
      setOtaResult(result);
      setOtaPhase("success");
      pushToast({
        tone: "success",
        title: `已推送到设备 · ${result.manifest?.name || otaTargetName}`,
      });
```

and inside the catch:

```jsx
    } catch (err) {
      const msg = typeof err === "string" ? err : String(err);
      if (!useSsh && (msg.includes("USB 未连接") || msg.includes("USB not connected"))) {
        setOtaPhase("waiting-usb");
      } else {
        setOtaError(msg);
        setOtaPhase("error");
        pushToast({ tone: "error", title: "安装失败", message: msg });
      }
    }
```

- [ ] **Step 5: Run test**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/ComponentCenter.test.js
```

Expected: ALL PASS. Both `tone: "success"` and `tone: "error"` patterns now match. `pet-manager:active-component` / `localStorage.setItem` / `new Event("storage")` all match. `component-tool-error`, `setClawpkgImportError`, `setSkillInstallError` are all gone.

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/ComponentCenter.jsx
git commit -m "feat(component-center): toast notifications + active-component localStorage write"
```

---

## Task 7: Smoke test, regression sweep, folder map update

**Files:**
- Modify: `ref/src/.folder.md` (refresh ComponentCenter row description)

- [ ] **Step 1: Run the full project test suite**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/ComponentCenter.test.js src/shell/*.test.js
```

Expected: ALL PASS.

Then run the whole project:

```bash
cd /path/to/claw-pet-manager/ref
node --test src/*.test.js src/lib/*.test.js src/shell/*.test.js
```

Expected: ALL PASS. If `AppearanceGallery.test.js` or `DeviceDashboard.test.js` fail with errors that mention strings unique to ComponentCenter (e.g. a cross-page assertion that no longer matches), that is a sign the other plans' tests overlap — investigate and fix only what's clearly Plan-4 caused.

- [ ] **Step 2: Manual smoke flow**

```bash
cd /path/to/claw-pet-manager/ref
npm run dev:web
```

In the app at `http://localhost:4173`:

1. Open sidebar "组件中心".
   - Page header shows "组件中心" + subtitle + "刷新草稿" + "创建组件" buttons.
   - Body: left = grid of built-ins (with "内置" badges) and drafts (with "自定义" badges and an "安装到设备" + "删除" pair), in that order. Right = preview Card with device-screen + bindings + install button.
2. Click any component card → preview updates.
3. Click the install button under the preview.
   - OTA modal appears (or SSH path completes immediately).
   - On success: toast pops up "已推送到设备 · ...", and the sidebar `ContextRail` (Plan 1) updates its 3rd row to show the just-installed component name.
4. Click "创建组件" in the header → right drawer slides in with the 3 STEP cards.
5. Click outside the drawer (backdrop) → drawer closes. Re-open, press `Escape` → drawer closes.
6. Click STEP 1 "一键安装 Skill" → toast appears ("Skill 已安装到 N 个…" or error). No inline `.component-tool-error` text inside the drawer.
7. Refresh page. The active component should still appear in the sidebar `ContextRail` (because `pet-manager:active-component` is persisted in localStorage, and `DeviceContext.jsx` reads it on mount).

Stop dev server (`Ctrl+C`).

- [ ] **Step 3: Update `ref/src/.folder.md` row for `ComponentCenter.jsx`**

Open `ref/src/.folder.md`, find the row for `ComponentCenter.jsx` in the Files table, and update its description so it mentions the new shape. Example replacement (adjust to match the file's actual existing wording — just keep the column alignment):

| Before | After |
|---|---|
| `ComponentCenter.jsx` | `component` | (old description mentioning 3-step tutorial inline + main panel + side panel) | → | `ComponentCenter.jsx` | `component` | 组件中心：消费 Plan 1 shell（PageShell / Card / useToast / useDeviceContext），主区组件库网格（内置在前，草稿在后，徽章区分），侧区始终可见安装预览，"创建组件" actions 按钮打开右侧 CreateComponentDrawer 抽屉容纳 STEP 1/2/3。安装成功后写入 `pet-manager:active-component` 以驱动侧栏 ContextRail 更新。 |

- [ ] **Step 4: Final commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/.folder.md
git commit -m "docs(component-center): refresh folder map for Plan 4 layout"
```

---

## Definition of Done for Plan 4

- [ ] `ref/src/ComponentCenter.jsx` consumes `<PageShell>`, `<Card>`, `useToast`, `useDeviceContext` from `ref/src/shell/`
- [ ] No file under `ref/src/shell/` was modified
- [ ] `<PageShell actions={...}>` holds the "刷新草稿" + "创建组件" buttons; clicking "创建组件" opens `<CreateComponentDrawer>`
- [ ] Body is `<div className="component-center-grid-layout">` with a left `<Card>` (grid: built-ins first, drafts after, both with corner badges) and a right `<aside className="component-center-preview-aside">` (always visible; soft-prompt empty state available)
- [ ] `CreateComponentDrawer` is a local component (NOT in `ref/src/shell/`), right-side slide-out, backdrop-clickable, Escape-closable, hosts the 3 STEP cards verbatim
- [ ] Inline `.component-tool-error` displays are gone — all surfaced via `useToast()`. `clawpkgImportError` and `skillInstallError` state are removed
- [ ] `usbConnected` local state is removed; OTA modal reads `usb.connected` from `useDeviceContext()`
- [ ] On successful install, `localStorage["pet-manager:active-component"]` is written as `{id, name}` and a `storage` event is dispatched — sidebar `ContextRail` (Plan 1) updates visibly
- [ ] All 3 modals (OTA / replace / delete / generated-prompt) still work; OTA additionally fires a toast on success/failure
- [ ] `node --test src/ComponentCenter.test.js` is GREEN
- [ ] `node --test src/*.test.js src/lib/*.test.js src/shell/*.test.js` is GREEN
- [ ] Manual smoke flow in Task 7 step 2 passes end-to-end

---

## Self-Review

**Spec coverage** (spec § "Plan 4: Component Center" + § "关键决定"):

| Spec requirement | Where implemented |
|---|---|
| `<PageShell title="组件中心" subtitle=...>` | Task 2 Step 4 |
| `actions=[<Button>刷新草稿</Button>, <Button icon={Sparkles}>创建组件</Button>]` (no chevron — drawer is the only sub-flow) | Task 2 Step 4 |
| Main-area Card containing 组件库网格 | Task 3 Step 1 |
| 内置在前、草稿在后 in same grid | Task 3 Step 1 (order: `MOCK_COMPONENT_CENTER.components.map` then `drafts.map`) |
| 同一种 card + 角徽章区分 | Task 3 Step 1 (`component-store-card__badge--builtin` / `--custom` preserved) |
| Side-area Card with 设备屏预览 + 组件名 + 描述 + 安装按钮 | Task 4 Step 1 |
| Side area always visible (soft prompt when nothing selected) | Task 4 Step 1 (the `selectedComponent ? <Card>preview</Card> : <Card><div className="component-center-preview-empty">选一个组件预览和安装</div></Card>` branch) |
| 3 步教程从主流变为 actions 抽屉 | Task 5 (drawer triggered from PageShell actions) |
| 抽屉内容复用现有 3 个 component-tool-card | Task 5 Step 2 (copied verbatim into `CreateComponentDrawer`) |
| Drawer backdrop click + ESC close | Task 5 Step 2 (`onClick={onClose}` on backdrop, `keydown Escape` listener) |
| Install drives sidebar `ContextRail` via `pet-manager:active-component` | Task 6 Step 1 |
| Replace `sync-notice` / inline errors with `useToast()` | Task 6 Steps 2-4 |
| Existing OTA / replace / delete / prompt modals preserved | Task 2 Step 4 keeps them; Task 6 Step 4 augments OTA with toast |
| Existing `MOCK_COMPONENT_CENTER` consumption unchanged | Task 3 grid map is identical; mock-data.js untouched |
| Existing `ComponentCenter.test.js` adapted (not deleted) | Task 1 |
| USB connection state for "安装到设备" from `useDeviceContext()` | Task 2 Step 3 |
| 刷新草稿 is its own action | Task 2 Step 4 first action |
| `ref/src/shell/` not modified | Enforced in "Definition of Done" and Conventions; no task touches shell |

**Placeholder scan:** none. Every step has concrete code, exact commands, exact commit messages. No "TBD", no "similar to above", no "implement error handling" hand-wavery.

**Type consistency check:**
- `useToast()` returns `{ push, dismiss, items }` (Plan 1 Task 4). We destructure as `{ push: pushToast }` to avoid shadowing the local `push` if any existed; all call sites use `pushToast({...})`. ✓
- `useDeviceContext()` returns `{ binding, usb, deviceOnline, ..., currentComponent, applyDesktopPet, refresh }` (Plan 1 Task 5). We consume only `{ usb }`. ✓
- `pet-manager:active-component` is the exact same string as `ACTIVE_COMPONENT_STORAGE_KEY` in Plan 1 `DeviceContext.jsx`. ✓
- `Event("storage")` is exactly what `DeviceContext.jsx` listens for. ✓
- `<PageShell title subtitle actions>` matches Plan 1 Task 2's prop signature. ✓
- `<Card title subtitle>` matches Plan 1 Task 3's prop signature. ✓
- The drawer is purely local — no shell prop names introduced. ✓

**Known fragile area:** Task 4 Step 1 changes `selectedComponent`'s fallback from "first built-in" to `null`. Every consumer is updated in the same step (5 call sites enumerated). If a later code change re-introduces a consumer that assumes non-null, the preview-empty branch should not be removed — it is required by the spec.

**Out-of-scope confirmation:** No Rust changes (`install_widget_skill`, `install_clawpkg_over_usb`, `install_clawpkg_over_ssh`, `delete_component_draft`, `list_component_drafts`, `usb_get_status` unchanged). No `mock-data.js` change. No shell file change. No App.jsx change. No lib helper change.

---
