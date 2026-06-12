# Plan 2: Device Dashboard Reorganization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `ref/src/DeviceDashboard.jsx` from its current 5-section layout into a 4-section layout (设备状态条 / 当前展示 / 按钮配置 / 语音助手) built on the Plan 1 shell, with two independent inputs for channel and formosa, and always-visible board button configuration whose SVG callouts now show the current action label.

**Architecture:** The dashboard becomes a thin composition layer on top of `<PageShell>` + `<Card>` + `<Card.Collapsible>`. It reads device/USB/appearance state from `useDeviceContext()` (deleting the file's local polling effects) and surfaces success / error / progress notices through `useToast()` (keeping in-context `message-banner`s for appearance load errors). The big `DesktopPetAssignmentPanel` is replaced by a lighter `CurrentDisplayCard` that exposes a 渠道 dropdown and a 「更换 ▾」 button (formosa picker). `VoiceAssistantPanel` is split into `BoardButtonPanel` (区 3, always visible) and a slimmer `VoiceAssistantPanel` (区 4, collapsible). The board SVG (`BoardButtonMap`) is upgraded to render each callout's current action label, not just highlight `voice_ptt`.

**Tech Stack:** React 18, lucide-react, vanilla CSS (existing tokens in `styles.css`), `node:test` for static-source-analysis tests (existing project pattern). No new dependencies.

**Spec reference:** `docs/superpowers/specs/2026-05-26-pet-manager-layout-redesign-design.md` — sections "Plan 2: Device Dashboard", "渠道与形象的独立切换", "按钮配置 SVG 的升级", "已删除的元素", "Out of Scope".

**Plan 1 dependency:** This plan assumes Plan 1 is merged and the shell API is frozen. Shell files under `ref/src/shell/` MUST NOT be modified by this plan. If a gap is discovered, stop and file a shell amendment PR per the spec's "并行执行的风险与对策" table.

---

## File Structure

**New files:**

| Path | Responsibility |
|------|----------------|
| `ref/src/dashboard/CurrentDisplayCard.jsx` | Region 2: large preview + 渠道 dropdown (independent) + 「更换 ▾」 button that opens the formosa picker |
| `ref/src/dashboard/CurrentDisplayCard.test.js` | Static source assertions for the card's structure, two independent inputs, USB-required routing per spec table |
| `ref/src/dashboard/BoardButtonPanel.jsx` | Region 3: always-visible SVG + 5 button-action rows + USB OTA button, extracted from the old `VoiceAssistantPanel` / `BoardButtonConfigPanel` |
| `ref/src/dashboard/BoardButtonPanel.test.js` | Static source assertions: SVG callout labels per button, hover row-highlight linkage, OTA dispatch wiring |
| `ref/src/dashboard/VoiceAssistantPanel.jsx` | Region 4: voice on/off switch + session select + start/stop listening button (no button-config inside) |
| `ref/src/dashboard/VoiceAssistantPanel.test.js` | Static source assertions: no embedded button config, controls flat, collapsible summary text |
| `ref/src/dashboard/DeviceStatusBar.jsx` | Region 1: single-row card with board id / WiFi / USB-online-offline chip |
| `ref/src/dashboard/DeviceStatusBar.test.js` | Static source assertions for the 3 connection states |
| `ref/src/dashboard/DashboardActionsMenu.jsx` | The right-side `<Menu>` mounted into `<PageShell actions>`: 发送测试消息 / 复制桌面设备 ID / 解绑设备 (danger) |
| `ref/src/dashboard/DashboardActionsMenu.test.js` | Static source assertions for the 3 menu items and the danger styling on 解绑 |
| `ref/src/dashboard/.folder.md` | Folder map for the new `dashboard/` subfolder |

**Modified files:**

| Path | What changes |
|------|--------------|
| `ref/src/DeviceDashboard.jsx` | Becomes the orchestrator: imports `PageShell` + `Card`(.Collapsible) + `useDeviceContext` + `useToast` + the 5 new dashboard subcomponents. Existing local polling `useEffect` blocks (USB poll, online poll, agents detect, bridge profile load, appearance load) are deleted — read from context instead. `applyDesktopPetSelection` becomes a thin wrapper around `useDeviceContext().applyDesktopPet`. `DesktopPetAssignmentPanel` / `AgentAppearancePickerModal` / `BoardButtonConfigPanel` / `VoiceAssistantPanel` / `BoardButtonMap` are removed from this file (split into the dashboard/ folder). Keep `ChannelSwitchConfirmModal` / `DeviceGuideModal` / voice-bus fetch helpers / voice config storage helpers / button option constants in this file (or move alongside the new dashboard files where they're used). |
| `ref/src/DeviceDashboard.test.js` | Adapt: keep behavioral assertions (channel switch persistence, USB-required appearance change, button OTA, voice placement), update selectors to the new component names. Add new assertions for: 4-section IA, 渠道 and 「更换 ▾」 as two independent inputs, SVG callouts show action labels. |
| `ref/src/styles.css` | Add `.dashboard-status-bar` / `.dashboard-current-display` / `.board-button-panel__callout-label` rules. Existing `.voice-button-action-list` / `.board-button-map__*` rules stay (still used). Delete dead rules (`.dashboard-runtime-card*`, `.desktop-pet-channel-card*`, `.desktop-pet-channel-expanded*`, `.desktop-pet-follow-list`, `.desktop-pet-assignment-panel`) ONLY if `grep` confirms they have no remaining references — defer otherwise per spec "CSS 旧 class 的最终清理 ... 延后处理". |
| `ref/src/.folder.md` | Append a row for `dashboard/` subfolder; refresh the `DeviceDashboard.jsx` / `DeviceDashboard.test.js` rows to reflect the 4-section IA. |

**Out of scope** (per spec): any Rust file, any `lib/*.js`, any file under `ref/src/shell/`, `AppearanceGallery.jsx`, `ComponentCenter.jsx`, `App.jsx`, `DeviceSetup.jsx`, `AppearancePreview.jsx`, `ChannelSwitchConfirmModal.jsx`, `DeviceGuideModal.jsx`.

---

## Conventions used in this plan

- **All commands assume `cd /path/to/claw-pet-manager/ref`** unless otherwise noted.
- **Test runner:** `node --test src/dashboard/<file>.test.js` (the project has no `test` npm script — invoke `node --test` directly).
- **Test style:** static source analysis via `readFileSync` + regex (existing project pattern; see `src/DeviceDashboard.test.js`). Do NOT introduce jest / RTL.
- **Commit style:** conventional, Chinese OK, no `--no-verify`. Pattern: `feat(dashboard): <one-line summary>` for code, `test(dashboard): ...` for test-only commits, `refactor(dashboard): ...` for behavior-preserving moves.
- **Spec table for 渠道 vs 形象 routing** (encoded in tests in Task 3):

| UI 入口 | agentId 变化 | appearance 变化 | USB 要求 | 是否弹 confirm |
|---|---|---|---|---|
| 渠道下拉切到 X，且 `map[X]` 与当前 formosa 相同 | 变 | 不变 | 在线/USB 任一 | 弹 |
| 渠道下拉切到 X，且 `map[X]` 与当前 formosa 不同 | 变 | 变 | 必须 USB | 弹 + 提示要 USB |
| 渠道下拉切到 X，且 `map[X]` 不存在 | 变 | 沿用当前并写进 map | 在线/USB 任一 | 不弹 |
| 「更换 ▾」选新形象 | 不变 | 变 | 必须 USB | 不弹 |

---

## Task 1: Scaffold the `dashboard/` folder

**Files:**
- Create: `ref/src/dashboard/.folder.md`
- Modify: `ref/src/.folder.md` (add `dashboard/` row to Files table)

- [ ] **Step 1: Create the dashboard folder marker**

Create `ref/src/dashboard/.folder.md` with this content:

```markdown
# Folder Plan: ref/src/dashboard

## Architecture
- Scope: ref/src/dashboard
- Function: Device-page subcomponents extracted from DeviceDashboard.jsx during the layout reorg — DeviceStatusBar (区 1), CurrentDisplayCard (区 2: 渠道 + 形象 两个独立入口), BoardButtonPanel (区 3: SVG + 5 行按钮 + USB OTA), VoiceAssistantPanel (区 4: 折叠态语音开关 + 会话续接 + 启动收听), DashboardActionsMenu (PageShell 右上 actions 菜单).
- Sync: if this folder changes, update this file immediately.
- Reuse: consumed only by `ref/src/DeviceDashboard.jsx`. State comes from `shell/DeviceContext.jsx` via `useDeviceContext()`; user notices come from `shell/ToastStack.jsx` via `useToast()`. Channel-switch confirm modal continues to live at `ref/src/ChannelSwitchConfirmModal.jsx`.

## Files
| File | Pos | Function |
|---|---|---|
| `DeviceStatusBar.jsx` | `component` | Region 1: single-row card with board id, WiFi SSID, and a connection chip (USB / 在线 / 离线) reading from useDeviceContext |
| `DeviceStatusBar.test.js` | `test` | Static Node coverage for the three connection-state branches and the documented class names |
| `CurrentDisplayCard.jsx` | `component` | Region 2: large appearance preview, 渠道 dropdown, 「更换 ▾」 picker button; routes channel changes through useDeviceContext().applyDesktopPet honoring the spec's 4-row decision table and pops ChannelSwitchConfirmModal when needed |
| `CurrentDisplayCard.test.js` | `test` | Static Node coverage that confirms two independent inputs, USB-required appearance change branch, confirm-modal channel branch, and applyDesktopPet invocation |
| `BoardButtonPanel.jsx` | `component` | Region 3: always-visible card hosting the upgraded BoardButtonMap (callout labels per button), the 5 button-action rows, and the USB OTA dispatch button |
| `BoardButtonPanel.test.js` | `test` | Static Node coverage that the SVG callouts render the current action label, the 5 BOARD_BUTTON_CONTROL_ROWS render as editable rows, voice_ptt rows show the voice-enabled chip, and OTA dispatch wiring stays |
| `VoiceAssistantPanel.jsx` | `component` | Region 4: lean voice section — voice on/off switch, 续接会话 select, 启动/停止板端收听 button. No embedded button config (moved to BoardButtonPanel). Designed to live inside `<Card.Collapsible>` |
| `VoiceAssistantPanel.test.js` | `test` | Static Node coverage that BoardButtonConfigPanel is no longer embedded, the voice on/off switch + session select + listen button are present, and the panel exposes a summary string for the collapsible header |
| `DashboardActionsMenu.jsx` | `component` | PageShell-actions menu with 发送测试消息 / 复制桌面设备 ID / 解绑设备 (danger) |
| `DashboardActionsMenu.test.js` | `test` | Static Node coverage for the 3 menu items, the danger styling on 解绑, and the callback prop signature |
```

- [ ] **Step 2: Append `dashboard/` row to parent `.folder.md`**

Open `ref/src/.folder.md`. In the Files table, add this row immediately after the row for `shell/` (which Plan 1 added) — or at the end of the table if `shell/` is not yet present:

```markdown
| `dashboard/` | `folder` | Device-page subcomponents — see `dashboard/.folder.md` |
```

- [ ] **Step 3: Verify and commit**

```bash
cd /path/to/claw-pet-manager/ref
ls src/dashboard/
```
Expected: `.folder.md`

```bash
cd /path/to/claw-pet-manager
git add ref/src/dashboard/.folder.md ref/src/.folder.md
git commit -m "feat(dashboard): 初始化 ref/src/dashboard 目录与 folder map"
```

---

## Task 2: DeviceStatusBar (区 1)

**Files:**
- Create: `ref/src/dashboard/DeviceStatusBar.jsx`
- Create: `ref/src/dashboard/DeviceStatusBar.test.js`
- Modify: `ref/src/styles.css` (add `.dashboard-status-bar` block)

- [ ] **Step 1: Write the failing test**

Create `ref/src/dashboard/DeviceStatusBar.test.js`:

```javascript
/**
 * [Input] Read DeviceStatusBar.jsx source.
 * [Output] Static Node coverage that the status bar reads from useDeviceContext, renders board id + WiFi + a chip that flips between USB / 在线 / 离线, and uses the documented class names.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "DeviceStatusBar.jsx"), "utf8");

test("DeviceStatusBar exports a default React component", () => {
  assert.match(source, /export default function DeviceStatusBar\s*\(/);
});

test("DeviceStatusBar consumes useDeviceContext (no local polling)", () => {
  assert.match(source, /useDeviceContext\(/);
  assert.doesNotMatch(source, /usb_get_status/);
  assert.doesNotMatch(source, /check_device_availability/);
});

test("DeviceStatusBar renders the 3 connection states", () => {
  // The three labels must appear so each branch is reachable.
  assert.match(source, /USB 直连|USB/);
  assert.match(source, /在线/);
  assert.match(source, /离线/);
});

test("DeviceStatusBar reads binding.boardDeviceId and binding.wifiSsid", () => {
  assert.match(source, /binding\.boardDeviceId/);
  assert.match(source, /binding\.wifiSsid/);
});

test("DeviceStatusBar uses the documented class names", () => {
  assert.match(source, /className="dashboard-status-bar/);
  assert.match(source, /dashboard-status-bar__chip/);
});
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/dashboard/DeviceStatusBar.test.js
```
Expected: ALL FAIL with ENOENT (file does not exist yet).

- [ ] **Step 3: Implement DeviceStatusBar**

Create `ref/src/dashboard/DeviceStatusBar.jsx`:

```jsx
/**
 * [Input] useDeviceContext for binding/usb/deviceOnline.
 * [Output] Region 1 of the device dashboard: single-row card showing board id + WiFi SSID + connection chip (USB / 在线 / 离线).
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React from "react";
import { Monitor, Usb, Wifi, WifiOff } from "lucide-react";
import { useDeviceContext } from "../shell/DeviceContext.jsx";

export default function DeviceStatusBar() {
  const { binding, usb, deviceOnline } = useDeviceContext();
  if (!binding) return null;

  let chipClass = "dashboard-status-bar__chip dashboard-status-bar__chip--warn";
  let chipIcon = <WifiOff size={14} />;
  let chipLabel = "离线";
  if (usb.connected) {
    chipClass = "dashboard-status-bar__chip dashboard-status-bar__chip--ok";
    chipIcon = <Usb size={14} />;
    chipLabel = "USB 直连";
  } else if (deviceOnline) {
    chipClass = "dashboard-status-bar__chip dashboard-status-bar__chip--ok";
    chipIcon = <Wifi size={14} />;
    chipLabel = "在线";
  }

  return (
    <div className="dashboard-status-bar">
      <span className="dashboard-status-bar__icon">
        <Monitor size={18} />
      </span>
      <div className="dashboard-status-bar__copy">
        <strong className="dashboard-status-bar__board-id">{binding.boardDeviceId}</strong>
        <span className="dashboard-status-bar__sub">
          WiFi: {binding.wifiSsid || "未知"}
        </span>
      </div>
      <span className={chipClass}>
        {chipIcon}
        {chipLabel}
      </span>
    </div>
  );
}
```

- [ ] **Step 4: Add CSS for the status bar**

In `ref/src/styles.css`, find the `/* === shell ===` section added by Plan 1. Immediately AFTER that section's closing comment / before the next top-level section, add a new `/* === dashboard ===` section with:

```css
/* === dashboard ======================================================== */

.dashboard-status-bar {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 14px;
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: var(--radius-md);
}

.dashboard-status-bar__icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 32px;
  height: 32px;
  border-radius: var(--radius-sm);
  background: var(--surface-muted);
  color: var(--text);
}

.dashboard-status-bar__copy {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
  flex: 1;
}

.dashboard-status-bar__board-id {
  font-size: 13px;
  font-weight: 600;
  color: var(--text);
}

.dashboard-status-bar__sub {
  font-size: 12px;
  color: var(--text-muted);
}

.dashboard-status-bar__chip {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 4px 10px;
  border-radius: var(--radius-pill);
  font-size: 12px;
  font-weight: 600;
}

.dashboard-status-bar__chip--ok {
  color: var(--success);
  background: var(--success-soft);
}

.dashboard-status-bar__chip--warn {
  color: var(--warning);
  background: var(--warning-soft);
}
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/dashboard/DeviceStatusBar.test.js
```
Expected: ALL PASS (5 tests).

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/dashboard/DeviceStatusBar.jsx ref/src/dashboard/DeviceStatusBar.test.js ref/src/styles.css
git commit -m "feat(dashboard): add DeviceStatusBar for region 1"
```

---

## Task 3: CurrentDisplayCard (区 2) — 渠道 + 「更换 ▾」 两个独立入口

This is the most behavior-rich task. The card has TWO independent inputs that map to the spec's 4-row routing table. We encode each row as a test and verify the implementation routes correctly through `applyDesktopPet` and `ChannelSwitchConfirmModal`.

**Files:**
- Create: `ref/src/dashboard/CurrentDisplayCard.jsx`
- Create: `ref/src/dashboard/CurrentDisplayCard.test.js`
- Modify: `ref/src/styles.css` (extend dashboard section with `.dashboard-current-display` rules)

- [ ] **Step 1: Write the failing test**

Create `ref/src/dashboard/CurrentDisplayCard.test.js`:

```javascript
/**
 * [Input] Read CurrentDisplayCard.jsx source.
 * [Output] Static Node coverage that confirms two independent inputs (渠道 dropdown, 「更换」 picker), the spec's 4-row routing decision encoded in handlers, USB-required gating for appearance changes, ChannelSwitchConfirmModal usage for channel switches, and applyDesktopPet wiring through useDeviceContext.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "CurrentDisplayCard.jsx"), "utf8");

test("CurrentDisplayCard exports a default React component", () => {
  assert.match(source, /export default function CurrentDisplayCard\s*\(/);
});

test("CurrentDisplayCard reads state from useDeviceContext (no local polling)", () => {
  assert.match(source, /useDeviceContext\(/);
  assert.doesNotMatch(source, /usb_get_status/);
  assert.doesNotMatch(source, /detect_local_agents/);
});

test("CurrentDisplayCard surfaces notices through useToast", () => {
  assert.match(source, /useToast\(/);
});

test("CurrentDisplayCard renders the large appearance preview via AppearancePreview", () => {
  assert.match(source, /AppearancePreview/);
});

test("CurrentDisplayCard has two independent inputs: a 渠道 <select> and a 「更换」 button", () => {
  // 渠道 input — native <select> bound to the channel handler.
  assert.match(source, /<select[^>]*className="dashboard-current-display__channel-select"/);
  assert.match(source, /onChannelSelect|handleChannelChange/);
  // 形象 input — the 「更换」 trigger that opens the formosa picker.
  assert.match(source, /更换/);
  assert.match(source, /onOpenFormosaPicker|openFormosaPicker|setPickerOpen/);
});

test("CurrentDisplayCard delegates apply to useDeviceContext().applyDesktopPet (no reimplementation)", () => {
  assert.match(source, /applyDesktopPet\(/);
  assert.doesNotMatch(source, /applyDesktopPetAssignment\(/); // do NOT call the lib directly
});

test("Spec row 1+2 — selecting a different channel pops ChannelSwitchConfirmModal", () => {
  assert.match(source, /shouldConfirmChannelSwitch\(/);
  assert.match(source, /ChannelSwitchConfirmModal/);
});

test("Spec row 4 — opening the formosa picker is a separate path that requires USB", () => {
  // The card asks the picker to gate the apply button by USB connectivity, mirroring spec row 4.
  assert.match(source, /usb\.connected/);
  assert.match(source, /APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE/);
});

test("Spec row 3 — switching to a channel without a remembered formosa carries the current one over", () => {
  // Encoded as: when map[nextAgentId] is missing, fall back to currentDisplay.appearance?.id.
  assert.match(source, /agentAppearanceMap\[nextAgentId\]|agentAppearanceMap\[agentId\]/);
  assert.match(source, /currentDisplay\.appearance\?\.id|currentDisplay\.appearance\.id/);
});

test("CurrentDisplayCard uses the documented class names", () => {
  assert.match(source, /className="dashboard-current-display"/);
  assert.match(source, /dashboard-current-display__preview/);
  assert.match(source, /dashboard-current-display__channel/);
  assert.match(source, /dashboard-current-display__formosa/);
});

test("CurrentDisplayCard reuses AgentAppearancePickerModal for the formosa picker (visual mode)", () => {
  // Spec: "复用现有 AgentAppearancePickerModal 视觉模式". The modal is named — either imported by name or referenced inline.
  assert.match(source, /AgentAppearancePickerModal|FormosaPickerModal/);
});
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/dashboard/CurrentDisplayCard.test.js
```
Expected: ALL FAIL (file missing).

- [ ] **Step 3: Implement CurrentDisplayCard (and the picker modal it reuses)**

Create `ref/src/dashboard/CurrentDisplayCard.jsx`:

```jsx
/**
 * [Input] useDeviceContext for binding/usb/appearances/agentAppearanceMap/agentOptions/currentDisplay/applyDesktopPet; useToast for notices.
 * [Output] Region 2 of the device dashboard: large preview + 渠道 dropdown (independent) + 「更换 ▾」 formosa picker; routes per spec § 渠道与形象的独立切换.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React, { useCallback, useState } from "react";
import { ChevronDown, ImagePlus, Loader, UploadCloud, X, CheckCircle } from "lucide-react";
import AppearancePreview from "../AppearancePreview.jsx";
import ChannelSwitchConfirmModal from "../ChannelSwitchConfirmModal.jsx";
import { resolveDashboardPreviewMedia } from "../lib/appearance-preview.js";
import {
  appearanceById,
  channelLabelForId,
  shouldConfirmChannelSwitch,
} from "../lib/agent-appearance-config.js";
import {
  APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE,
  CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE,
} from "../lib/desktop-pet-assignment.js";
import { useDeviceContext } from "../shell/DeviceContext.jsx";
import { useToast } from "../shell/ToastStack.jsx";

export default function CurrentDisplayCard() {
  const {
    usb,
    deviceOnline,
    appearances,
    agentAppearanceMap,
    agentOptions,
    currentDisplay,
    applyDesktopPet,
  } = useDeviceContext();
  const { push } = useToast();

  const [pickerOpen, setPickerOpen] = useState(false);
  const [pendingChannel, setPendingChannel] = useState(null);
  const [syncing, setSyncing] = useState(false);

  const currentAppearance = currentDisplay.appearance;
  const previewMedia = resolveDashboardPreviewMedia(currentAppearance);
  const currentAgentId = currentDisplay.agentId;
  const detectedAgents = agentOptions.filter((a) => a.detected || a.id === currentAgentId);

  // --- Channel input -------------------------------------------------------
  const performChannelChange = useCallback(
    async (nextAgentId) => {
      const nextAppearanceId =
        agentAppearanceMap[nextAgentId] || currentDisplay.appearance?.id || "";
      const nextAppearance = appearanceById(appearances, nextAppearanceId);
      if (!nextAppearance) {
        push({ tone: "warning", title: "请选择一个形象后再切换渠道" });
        return;
      }
      setSyncing(true);
      try {
        const { notice } = await applyDesktopPet(nextAgentId, nextAppearance, {
          onProgress: (p) => push({ tone: "info", title: p.text, ttl: 2000 }),
        });
        push({ tone: "success", title: notice });
      } catch (err) {
        const msg = err?.message || String(err);
        const tone =
          msg === APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE ||
          msg === CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE
            ? "warning"
            : "error";
        push({ tone, title: msg });
      } finally {
        setSyncing(false);
      }
    },
    [agentAppearanceMap, appearances, applyDesktopPet, currentDisplay, push],
  );

  const handleChannelChange = useCallback(
    (nextAgentId) => {
      if (!nextAgentId || nextAgentId === currentAgentId) return;
      // Spec row 1+2: switching channel triggers confirm modal.
      if (shouldConfirmChannelSwitch(agentAppearanceMap, nextAgentId, new Set([currentAgentId]))) {
        setPendingChannel(nextAgentId);
        return;
      }
      // Spec row 3: no prior current channel — apply directly.
      performChannelChange(nextAgentId);
    },
    [agentAppearanceMap, currentAgentId, performChannelChange],
  );

  const confirmPendingChannel = useCallback(() => {
    if (pendingChannel) performChannelChange(pendingChannel);
    setPendingChannel(null);
  }, [pendingChannel, performChannelChange]);

  // --- Formosa input (independent) -----------------------------------------
  const performFormosaChange = useCallback(
    async (nextAppearanceId) => {
      const nextAppearance = appearanceById(appearances, nextAppearanceId);
      if (!nextAppearance || !currentAgentId) return;
      setSyncing(true);
      try {
        const { notice } = await applyDesktopPet(currentAgentId, nextAppearance, {
          onProgress: (p) => push({ tone: "info", title: p.text, ttl: 2000 }),
        });
        push({ tone: "success", title: notice });
        setPickerOpen(false);
      } catch (err) {
        const msg = err?.message || String(err);
        const tone = msg === APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE ? "warning" : "error";
        push({ tone, title: msg });
      } finally {
        setSyncing(false);
      }
    },
    [appearances, applyDesktopPet, currentAgentId, push],
  );

  // Spec row 4: formosa change requires USB.
  const formosaCanApply = Boolean(usb.connected);
  const formosaGateMessage = formosaCanApply ? "" : APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE;

  return (
    <div className="dashboard-current-display">
      <div className="dashboard-current-display__preview">
        {currentAppearance ? (
          <>
            <span className="appearance-thumb__badge">
              {currentAppearance.type === "codex-import"
                ? "codex pet"
                : currentAppearance.type === "builtin"
                  ? "内置形象"
                  : "自定义形象"}
            </span>
            <AppearancePreview
              media={previewMedia}
              className="dashboard-current-display__preview-media"
              emptyClassName="dashboard-current-display__preview-empty"
              playing
            />
          </>
        ) : (
          <div className="dashboard-current-display__preview-empty">
            <ImagePlus size={20} />
          </div>
        )}
      </div>

      <div className="dashboard-current-display__inputs">
        <label className="dashboard-current-display__channel">
          <span>渠道</span>
          <span className="dashboard-current-display__select-shell">
            <select
              className="dashboard-current-display__channel-select"
              value={currentAgentId}
              onChange={(event) => handleChannelChange(event.target.value)}
              disabled={syncing}
            >
              {detectedAgents.map((agent) => (
                <option key={agent.id} value={agent.id}>
                  {agent.label}
                </option>
              ))}
            </select>
            <ChevronDown size={14} aria-hidden="true" />
          </span>
        </label>

        <div className="dashboard-current-display__formosa">
          <span>形象</span>
          <div className="dashboard-current-display__formosa-row">
            <strong>{currentAppearance?.name || "未选择形象"}</strong>
            <button
              type="button"
              className="btn-secondary btn-sm"
              onClick={() => setPickerOpen(true)}
              disabled={syncing}
            >
              更换
              <ChevronDown size={14} aria-hidden="true" />
            </button>
          </div>
          {!usb.connected && (
            <p className="dashboard-current-display__hint">
              更换形象需要 USB 直连设备。当前{deviceOnline ? "在线（仅 WiFi）" : "离线"}。
            </p>
          )}
        </div>
      </div>

      {pickerOpen && (
        <AgentAppearancePickerModal
          appearances={appearances}
          selectedAppearanceId={currentAppearance?.id || ""}
          syncing={syncing}
          deviceConnected={formosaCanApply}
          deviceConnectionMessage={formosaGateMessage}
          onClose={() => setPickerOpen(false)}
          onPick={(appearanceId) => performFormosaChange(appearanceId)}
        />
      )}

      {pendingChannel && (
        <ChannelSwitchConfirmModal
          currentLabel={channelLabelForId(agentOptions, currentAgentId)}
          nextLabel={channelLabelForId(agentOptions, pendingChannel)}
          onCancel={() => setPendingChannel(null)}
          onConfirm={confirmPendingChannel}
        />
      )}
    </div>
  );
}

function AgentAppearancePickerModal({
  appearances,
  selectedAppearanceId,
  syncing,
  deviceConnected,
  deviceConnectionMessage,
  onPick,
  onClose,
}) {
  const [draftId, setDraftId] = useState(selectedAppearanceId || "");
  const canApply = !syncing && draftId && deviceConnected;
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div
        className="modal-card modal-card--appearance-picker"
        onClick={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <div>
            <h3 className="modal-title">选择设备展示形象</h3>
            <div className="modal-subtitle">仅替换当前渠道下的形象，不变更渠道。</div>
          </div>
          <button className="icon-btn" onClick={onClose} aria-label="关闭">
            <X size={16} />
          </button>
        </div>
        <div className="modal-body agent-appearance-picker-modal__body">
          <div className="agent-appearance-picker-modal__list">
            {appearances.map((row) => (
              <AgentAppearancePickerOption
                key={row.id}
                row={row}
                selected={draftId === row.id}
                disabled={syncing}
                onPick={() => setDraftId(row.id)}
              />
            ))}
          </div>
          {!deviceConnected && (
            <div className="message-banner message-banner--warning agent-appearance-picker-modal__notice">
              {deviceConnectionMessage}
            </div>
          )}
        </div>
        <div className="agent-appearance-picker-modal__actions">
          <button className="btn-secondary" type="button" onClick={onClose} disabled={syncing}>
            取消
          </button>
          <button
            className="btn-primary"
            type="button"
            onClick={() => canApply && onPick(draftId)}
            disabled={!canApply}
            title={!deviceConnected ? deviceConnectionMessage : undefined}
          >
            {syncing ? <Loader size={14} className="spin" /> : <UploadCloud size={14} />}
            {syncing ? "应用中…" : "设为桌宠"}
          </button>
        </div>
      </div>
    </div>
  );
}

function AgentAppearancePickerOption({ row, selected, disabled, onPick }) {
  const previewMedia = resolveDashboardPreviewMedia(row);
  return (
    <button
      type="button"
      className={`agent-appearance-picker-modal__option${selected ? " is-selected" : ""}`}
      onClick={onPick}
      disabled={disabled}
    >
      <span className="agent-appearance-picker-modal__preview">
        <span className="appearance-thumb__badge">
          {row.type === "codex-import"
            ? "codex pet"
            : row.type === "builtin"
              ? "内置形象"
              : "自定义形象"}
        </span>
        <AppearancePreview
          media={previewMedia}
          className="appearance-channel-preview__media"
          emptyClassName="appearance-channel-preview__empty"
        />
      </span>
      <span className="agent-appearance-picker-modal__copy">
        <strong>{row.name}</strong>
        <span>{row.description || `${row.provider} · ${row.model || "—"}`}</span>
      </span>
      {selected && <CheckCircle className="agent-appearance-picker-modal__check" size={18} />}
    </button>
  );
}
```

- [ ] **Step 4: Add CSS for the current-display card**

In `ref/src/styles.css`, inside the `/* === dashboard ===` section added in Task 2, append:

```css
.dashboard-current-display {
  display: grid;
  grid-template-columns: minmax(160px, 200px) 1fr;
  gap: 18px;
  align-items: start;
}

.dashboard-current-display__preview {
  position: relative;
  width: 100%;
  aspect-ratio: 4 / 3;
  background: var(--surface-muted);
  border-radius: var(--radius-md);
  overflow: hidden;
  display: flex;
  align-items: center;
  justify-content: center;
}

.dashboard-current-display__preview-media,
.dashboard-current-display__preview-empty {
  width: 100%;
  height: 100%;
  object-fit: cover;
  display: flex;
  align-items: center;
  justify-content: center;
  color: var(--text-muted);
}

.dashboard-current-display__inputs {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.dashboard-current-display__channel,
.dashboard-current-display__formosa {
  display: flex;
  flex-direction: column;
  gap: 4px;
  font-size: 12px;
  color: var(--text-muted);
}

.dashboard-current-display__select-shell {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 4px 8px;
  border: 1px solid var(--line);
  border-radius: var(--radius-sm);
  background: var(--surface);
}

.dashboard-current-display__channel-select {
  appearance: none;
  border: none;
  background: transparent;
  font-size: 13px;
  color: var(--text);
  min-width: 120px;
}

.dashboard-current-display__formosa-row {
  display: flex;
  align-items: center;
  gap: 8px;
}

.dashboard-current-display__hint {
  margin: 0;
  font-size: 11px;
  color: var(--warning);
}

@media (max-width: 720px) {
  .dashboard-current-display {
    grid-template-columns: 1fr;
  }
}
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/dashboard/CurrentDisplayCard.test.js
```
Expected: ALL PASS (11 tests).

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/dashboard/CurrentDisplayCard.jsx ref/src/dashboard/CurrentDisplayCard.test.js ref/src/styles.css
git commit -m "feat(dashboard): add CurrentDisplayCard with independent 渠道/形象 inputs"
```

---

## Task 4: BoardButtonPanel (区 3) — SVG callouts show current action labels

The existing `BoardButtonMap` in `DeviceDashboard.jsx` only highlights `voice_ptt`. Spec § "按钮配置 SVG 的升级" demands: each callout shows the current action label, plus hover on SVG button highlights the row below. The 5 editable rows + USB OTA button stay (they're already in `BoardButtonConfigPanel`).

**Files:**
- Create: `ref/src/dashboard/BoardButtonPanel.jsx`
- Create: `ref/src/dashboard/BoardButtonPanel.test.js`
- Modify: `ref/src/styles.css` (add `.board-button-panel__callout-label` rules)

- [ ] **Step 1: Write the failing test**

Create `ref/src/dashboard/BoardButtonPanel.test.js`:

```javascript
/**
 * [Input] Read BoardButtonPanel.jsx source.
 * [Output] Static Node coverage that the SVG renders each callout's current action label (not only voice_ptt highlight), the 5 editable rows persist, hover row-highlight wiring exists, voice_ptt row carries the voice-enabled chip, and USB OTA dispatch wiring stays intact.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "BoardButtonPanel.jsx"), "utf8");

test("BoardButtonPanel exports a default React component", () => {
  assert.match(source, /export default function BoardButtonPanel\s*\(/);
});

test("BoardButtonPanel renders the SVG board map with viewBox preserved", () => {
  assert.match(source, /viewBox="0 0 420 220"/);
  assert.match(source, /board-button-map__device/);
});

test("Each callout shows the current action label (spec § 按钮配置 SVG 的升级)", () => {
  // Action label rendered as <text> nodes anchored at each callout endpoint.
  assert.match(source, /board-button-panel__callout-label/);
  // The set of callout-labelled buttons must include all 5 control rows.
  assert.match(source, /top_button/);
  assert.match(source, /encoder_button/);
  assert.match(source, /encoder_rotate/);
  assert.match(source, /screen_tap/);
  assert.match(source, /screen_long_press/);
});

test("Hover effects respect prefers-reduced-motion", () => {
  // CSS-side: transitions must be killed under reduce-motion preference.
  const css = readFileSync(join(here, "..", "styles.css"), "utf8");
  assert.match(css, /@media\s*\(prefers-reduced-motion:\s*reduce\)/);
  assert.match(css, /board-button-panel__callout-label[\s\S]*transition:\s*none/);
});

test("Hovering an SVG callout highlights the matching editor row", () => {
  // Implementation hook: a state `hoveredButtonId` plus pointer events on the SVG button shapes,
  // plus an `is-hovered` class on the matching row.
  assert.match(source, /hoveredButtonId|setHoveredButton/);
  assert.match(source, /is-hovered/);
});

test("The 5 editable rows persist and use BOARD_BUTTON_CONTROL_ROWS + BUTTON_FUNCTION_OPTIONS", () => {
  assert.match(source, /BOARD_BUTTON_CONTROL_ROWS\.map/);
  assert.match(source, /BUTTON_FUNCTION_OPTIONS\.filter/);
  assert.match(source, /voice-button-action-list/);
  assert.match(source, /voice-button-action-select/);
});

test("Voice_ptt rows display a chip reflecting voice-enabled state", () => {
  assert.match(source, /语音助手已开启|语音已开启/);
  assert.match(source, /未开启/);
  // chip rendering must be conditional on the row's action being voice_ptt
  assert.match(source, /voice_ptt/);
});

test("USB OTA dispatch wiring stays — calls onApplyVoiceConfig and shows the button", () => {
  assert.match(source, /onApplyVoiceConfig/);
  assert.match(source, /通过 USB OTA 下发按钮配置/);
  assert.match(source, /需 USB OTA 生效/);
});

test("BoardButtonPanel is always visible (no Card.Collapsible wrapper in this file)", () => {
  // The panel itself does not collapse — the parent DeviceDashboard places it in a plain Card.
  assert.doesNotMatch(source, /Card\.Collapsible/);
});
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/dashboard/BoardButtonPanel.test.js
```
Expected: ALL FAIL (file missing).

- [ ] **Step 3: Implement BoardButtonPanel (with the upgraded SVG)**

Create `ref/src/dashboard/BoardButtonPanel.jsx`:

```jsx
/**
 * [Input] voiceConfig + buttonActions + voiceConfigDirty + voiceConfigOtaState + usbConnected + selectedTrigger + onVoiceConfigChange + onApplyVoiceConfig.
 * [Output] Region 3 of the device dashboard: always-visible SVG (callout per button shows current action label) + 5 editable rows + USB OTA dispatch button. Hovering an SVG button highlights its row below.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React, { useState } from "react";
import { ChevronDown, Loader, UploadCloud, Mic } from "lucide-react";
import {
  BOARD_BUTTON_CONTROL_ROWS,
  BUTTON_FUNCTION_OPTIONS,
  DEFAULT_BUTTON_ACTIONS,
  DEFAULT_VOICE_CONFIG,
  actionOptionById,
} from "../DeviceDashboard.jsx";

// Each callout has an (x,y) anchor in the SVG viewport where the label is drawn.
const CALLOUT_ANCHORS = {
  top_button:        { x: 112, y: 12,  align: "end"   },
  encoder_button:    { x: 386, y: 72,  align: "start" },
  encoder_rotate:    { x: 386, y: 110, align: "start" },
  screen_tap:        { x: 65,  y: 175, align: "start" },
  screen_long_press: { x: 225, y: 175, align: "end"   },
};

// Callout path d-attributes — endpoint coordinates match the visual button center.
const CALLOUT_PATHS = {
  top_button:        "M128 34 L112 12",
  encoder_button:    "M314 95 L386 72",
  encoder_rotate:    "M314 95 L386 110",
  screen_tap:        "M120 113 L65 170",
  screen_long_press: "M180 113 L225 170",
};

export default function BoardButtonPanel({
  voiceConfig,
  buttonActions = DEFAULT_BUTTON_ACTIONS,
  voiceConfigDirty,
  voiceConfigOtaState,
  usbConnected,
  selectedTrigger,
  onVoiceConfigChange,
  onApplyVoiceConfig,
}) {
  const [hoveredButtonId, setHoveredButton] = useState("");
  const triggerId = selectedTrigger?.id || DEFAULT_VOICE_CONFIG.trigger;

  const onButtonActionChange = (row, actionId) => {
    const nextActions = { ...(buttonActions || DEFAULT_BUTTON_ACTIONS), [row.id]: actionId };
    const patch = { buttonActions: nextActions };
    if (actionId === "voice_ptt" && row.voiceTriggerId) {
      BOARD_BUTTON_CONTROL_ROWS.forEach((item) => {
        if (item.voiceTriggerId && item.id !== row.id && nextActions[item.id] === "voice_ptt") {
          nextActions[item.id] = item.defaultAction;
        }
      });
      patch.trigger = row.voiceTriggerId;
    } else if (row.voiceTriggerId && triggerId === row.voiceTriggerId) {
      const fallbackVoiceRow = BOARD_BUTTON_CONTROL_ROWS.find(
        (item) => item.voiceTriggerId && nextActions[item.id] === "voice_ptt",
      );
      patch.trigger = fallbackVoiceRow?.voiceTriggerId || DEFAULT_VOICE_CONFIG.trigger;
      if (!fallbackVoiceRow) nextActions.top_button = "voice_ptt";
    }
    onVoiceConfigChange(patch);
  };

  const messageClass = voiceConfigOtaState?.tone === "error"
    ? "message-banner--error"
    : voiceConfigOtaState?.tone === "success"
      ? "message-banner--success"
      : "message-banner--warning";

  return (
    <div className="board-button-panel" data-testid="board-button-config-card">
      <svg
        className="board-button-map__device board-button-panel__svg"
        viewBox="0 0 420 220"
        role="img"
        aria-label="板端外观和按钮位置示意"
      >
        <rect className="board-button-map__body" x="18" y="36" width="372" height="156" rx="32" />
        <rect className="board-button-map__screen-bezel" x="46" y="58" width="198" height="110" rx="18" />
        <rect className="board-button-map__screen" x="65" y="73" width="160" height="80" rx="8" />

        {/* Hit-areas for the 5 buttons. onMouseEnter sets hoveredButtonId so the row below highlights. */}
        <circle
          className={`board-button-map__top-button${buttonActions.top_button === "voice_ptt" && voiceConfig.enabled ? " is-active" : ""}`}
          cx="128" cy="34" r="19"
          onMouseEnter={() => setHoveredButton("top_button")}
          onMouseLeave={() => setHoveredButton("")}
          data-button-id="top_button"
        />
        <circle
          className={`board-button-map__encoder${buttonActions.encoder_button === "voice_ptt" && voiceConfig.enabled ? " is-active" : ""}`}
          cx="314" cy="95" r="48"
          onMouseEnter={() => setHoveredButton("encoder_button")}
          onMouseLeave={() => setHoveredButton("")}
          data-button-id="encoder_button"
        />
        <rect
          className="board-button-panel__screen-tap-hit"
          x="65" y="73" width="80" height="80"
          fill="transparent" pointerEvents="all"
          onMouseEnter={() => setHoveredButton("screen_tap")}
          onMouseLeave={() => setHoveredButton("")}
          data-button-id="screen_tap"
        />
        <rect
          className="board-button-panel__screen-long-hit"
          x="145" y="73" width="80" height="80"
          fill="transparent" pointerEvents="all"
          onMouseEnter={() => setHoveredButton("screen_long_press")}
          onMouseLeave={() => setHoveredButton("")}
          data-button-id="screen_long_press"
        />

        {/* Callout paths from each button to its label anchor. */}
        {Object.entries(CALLOUT_PATHS).map(([id, d]) => (
          <path key={id} className={`board-button-map__callout${hoveredButtonId === id ? " is-hovered" : ""}`} d={d} />
        ))}

        {/* Action labels next to each callout — the spec upgrade. */}
        {BOARD_BUTTON_CONTROL_ROWS.map((row) => {
          const anchor = CALLOUT_ANCHORS[row.id];
          if (!anchor) return null;
          const action = actionOptionById(buttonActions[row.id] || row.defaultAction);
          return (
            <text
              key={row.id}
              className={`board-button-panel__callout-label${hoveredButtonId === row.id ? " is-hovered" : ""}`}
              x={anchor.x}
              y={anchor.y}
              textAnchor={anchor.align}
              data-button-id={row.id}
            >
              {row.label}: {action.label}
            </text>
          );
        })}
      </svg>

      <div className="voice-button-action-list">
        <div className="voice-config-field__head">
          <span>按钮功能</span>
          <small>需 USB OTA 生效</small>
        </div>
        {BOARD_BUTTON_CONTROL_ROWS.map((row) => {
          const currentActionId = buttonActions?.[row.id] || row.defaultAction;
          const currentAction = actionOptionById(currentActionId);
          const allowedOptions = BUTTON_FUNCTION_OPTIONS.filter((option) =>
            row.actionOptions.includes(option.id),
          );
          const isVoicePttRow = currentActionId === "voice_ptt";
          const rowClass = `voice-button-action-row${hoveredButtonId === row.id ? " is-hovered" : ""}`;
          return (
            <label
              className={rowClass}
              key={row.id}
              onMouseEnter={() => setHoveredButton(row.id)}
              onMouseLeave={() => setHoveredButton("")}
            >
              <span className="voice-button-action-row__copy">
                <strong>{row.label}</strong>
                <small>{row.event}</small>
              </span>
              <span className="voice-button-action-select-shell">
                <select
                  className="voice-button-action-select"
                  value={currentActionId}
                  onChange={(event) => onButtonActionChange(row, event.target.value)}
                >
                  {allowedOptions.map((option) => (
                    <option key={option.id} value={option.id}>
                      {option.label}
                    </option>
                  ))}
                </select>
                <ChevronDown
                  className="voice-button-action-select-shell__chevron"
                  size={14}
                  aria-hidden="true"
                />
              </span>
              {isVoicePttRow && (
                <span className={`board-button-panel__voice-chip${voiceConfig.enabled ? " is-on" : ""}`}>
                  <Mic size={12} />
                  {voiceConfig.enabled ? "语音助手已开启" : "语音助手未开启"}
                </span>
              )}
              <small className="voice-button-action-row__detail">{currentAction.detail}</small>
            </label>
          );
        })}
      </div>

      <div className="voice-config-footer">
        <button
          type="button"
          className="btn-secondary btn-sm"
          onClick={onApplyVoiceConfig}
          disabled={voiceConfigOtaState?.pending || !usbConnected}
          title={usbConnected ? "" : "需要 USB 连接设备后下发"}
        >
          {voiceConfigOtaState?.pending ? (
            <Loader size={14} className="spin" />
          ) : (
            <UploadCloud size={14} />
          )}
          通过 USB OTA 下发按钮配置
        </button>
        <span className={`voice-config-ota-note${voiceConfigDirty ? " is-dirty" : ""}`}>
          {voiceConfigDirty ? "有未下发配置" : "按钮配置已保存"}
        </span>
      </div>

      {voiceConfigOtaState?.message && (
        <div className={`message-banner voice-config-message ${messageClass}`}>
          {voiceConfigOtaState.message}
        </div>
      )}
    </div>
  );
}
```

Note: `BOARD_BUTTON_CONTROL_ROWS`, `BUTTON_FUNCTION_OPTIONS`, `DEFAULT_BUTTON_ACTIONS`, `DEFAULT_VOICE_CONFIG`, and `actionOptionById` are imported from `../DeviceDashboard.jsx`. In Task 7 we will move these constants to a small `dashboard/constants.js` once the orchestrator file is rewritten; until then they live in `DeviceDashboard.jsx` and the panel reads them via the cross-file import.

- [ ] **Step 4: Add CSS for callout labels and hover sync**

In `ref/src/styles.css`, inside the `/* === dashboard ===` section, append:

```css
.board-button-panel {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.board-button-panel__svg {
  width: 100%;
  max-height: 260px;
}

.board-button-panel__callout-label {
  font-size: 10px;
  font-weight: 600;
  fill: var(--text-muted);
  pointer-events: none;
  transition: fill 120ms ease;
}

.board-button-panel__callout-label.is-hovered {
  fill: var(--accent);
}

.board-button-map__callout.is-hovered {
  stroke: var(--accent);
  stroke-width: 1.5;
}

.voice-button-action-row.is-hovered {
  background: var(--surface-muted);
  border-radius: var(--radius-sm);
}

.board-button-panel__voice-chip {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 2px 6px;
  border-radius: var(--radius-pill);
  font-size: 11px;
  font-weight: 600;
  color: var(--text-muted);
  background: var(--surface-muted);
}

.board-button-panel__voice-chip.is-on {
  color: var(--success);
  background: var(--success-soft);
}

/* Respect user's motion preference: keep the hover highlight (still informative)
   but kill the transition so nothing animates for users who opted out. */
@media (prefers-reduced-motion: reduce) {
  .board-button-panel__callout-label,
  .board-button-map__callout,
  .voice-button-action-row {
    transition: none !important;
  }
}
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/dashboard/BoardButtonPanel.test.js
```
Expected: ALL PASS (8 tests).

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/dashboard/BoardButtonPanel.jsx ref/src/dashboard/BoardButtonPanel.test.js ref/src/styles.css
git commit -m "feat(dashboard): add BoardButtonPanel with action-labelled SVG callouts"
```

---

## Task 5: VoiceAssistantPanel (区 4, collapsible)

The voice card no longer hosts button config. It keeps: voice on/off switch + 续接 session select + 启动/停止板端收听 button. It's designed to render INSIDE `<Card.Collapsible>` (the parent owns the wrapper).

**Files:**
- Create: `ref/src/dashboard/VoiceAssistantPanel.jsx`
- Create: `ref/src/dashboard/VoiceAssistantPanel.test.js`

- [ ] **Step 1: Write the failing test**

Create `ref/src/dashboard/VoiceAssistantPanel.test.js`:

```javascript
/**
 * [Input] Read VoiceAssistantPanel.jsx source.
 * [Output] Static Node coverage that the voice panel no longer embeds button config, exposes the 3 controls (switch / session / listen), is suitable for placement inside Card.Collapsible, and provides a summary helper.
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

test("Exports a helper for the Card.Collapsible summary string", () => {
  assert.match(source, /export function buildVoiceSummary\s*\(/);
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
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/dashboard/VoiceAssistantPanel.test.js
```
Expected: ALL FAIL (file missing).

- [ ] **Step 3: Implement VoiceAssistantPanel**

Create `ref/src/dashboard/VoiceAssistantPanel.jsx`:

```jsx
/**
 * [Input] state (busStatus/busSessions/busSessionId/voiceRuntime/audioBridge*/selectedAgentId/deviceOnline) + dispatch + toggleAudioBridge + voiceConfig + selectedTrigger + onVoiceConfigChange.
 * [Output] Region 4 of the device dashboard: lean voice panel — on/off switch + 续接会话 select + 启动/停止板端收听 button. Renders inside <Card.Collapsible>; the parent owns the wrapper.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React from "react";
import { ChevronDown, Loader, Mic, MicOff } from "lucide-react";

export function buildVoiceSummary(voiceConfig, selectedTrigger) {
  if (!voiceConfig?.enabled) return "未开启";
  const trigger = selectedTrigger?.label || "默认触发";
  return `已开启 · ${trigger}`;
}

export default function VoiceAssistantPanel({
  state,
  dispatch,
  toggleAudioBridge,
  voiceConfig,
  selectedTrigger,
  onVoiceConfigChange,
}) {
  const agents = Array.isArray(state.busStatus?.agents) ? state.busStatus.agents : [];
  const selectedAgent = agents.find((agent) => agent.agentId === state.selectedAgentId) || null;
  const ready = selectedAgent?.ready === true;
  const sessions = Array.isArray(state.busSessions) ? state.busSessions : [];

  const voiceRunning = state.voiceRuntime?.running === true;
  let audioBlockingReason = null;
  if (!state.selectedAgentId) {
    audioBlockingReason = "请先在「当前展示」里选择一个渠道";
  } else if (state.busStatus != null && !ready) {
    audioBlockingReason = selectedAgent?.reason || "语音 agent 未就绪";
  }

  const boardOffline = state.deviceOnline === false;

  return (
    <div className="voice-panel voice-panel--lean">
      <label className={`voice-config-switch${voiceConfig.enabled ? " is-on" : ""}`}>
        <input
          type="checkbox"
          checked={voiceConfig.enabled}
          onChange={(event) => onVoiceConfigChange({ enabled: event.target.checked })}
        />
        <span className="voice-config-switch__track" aria-hidden="true">
          <span className="voice-config-switch__thumb" />
        </span>
        <span className="voice-config-switch__copy">
          <strong>是否开启语音</strong>
          <span>
            {voiceConfig.enabled ? "板端语音已在客户端启用" : "关闭时不会启动板端麦克风收听"}
          </span>
        </span>
      </label>

      <label className="voice-session-field">
        <span>续接会话</span>
        <span className="voice-session-select-shell">
          <select
            className="voice-session-select"
            value={state.busSessionId}
            disabled={!ready || sessions.length === 0}
            onChange={(event) => {
              const value = event.target.value || "auto";
              dispatch({ type: "set_bus_session_id", value });
              try {
                if (value && value !== "auto") {
                  localStorage.setItem(
                    `pet-manager.voice-session.${state.selectedAgentId}`,
                    value,
                  );
                } else {
                  localStorage.removeItem(`pet-manager.voice-session.${state.selectedAgentId}`);
                }
              } catch {
                // ignore storage errors
              }
            }}
          >
            <option value="auto">最近的会话（自动）</option>
            {sessions.map((session) => {
              const ts =
                Number(session.lastModified) > 0
                  ? new Date(session.lastModified).toLocaleString()
                  : "";
              const label = session.summary || session.cwd || session.id;
              return (
                <option key={session.id} value={session.id}>
                  {label}
                  {ts ? `  ·  ${ts}` : ""}
                </option>
              );
            })}
          </select>
          <ChevronDown
            className="voice-session-select-shell__chevron"
            size={16}
            aria-hidden="true"
          />
        </span>
      </label>

      <div className="voice-action-row">
        {state.audioBridgeEnabled ? (
          <button
            type="button"
            className="btn-primary"
            onClick={() => toggleAudioBridge("stop")}
            disabled={state.audioBridgePending}
          >
            <MicOff size={16} />
            停止板端收听
          </button>
        ) : (
          <button
            type="button"
            className="btn-primary"
            onClick={() => toggleAudioBridge("start")}
            disabled={
              state.audioBridgePending || !!audioBlockingReason || !voiceConfig.enabled
            }
            title={
              !voiceConfig.enabled
                ? "请先开启板端语音"
                : audioBlockingReason || "启动前会自动检查本地 Bridge 和 voice-service。"
            }
          >
            <Mic size={16} />
            启动板端麦克风收听
          </button>
        )}

        {state.audioBridgePending && (
          <span className="voice-inline-status">
            <Loader size={14} className="spin" />
            正在下发信令...
          </span>
        )}
      </div>

      {boardOffline && !audioBlockingReason && voiceConfig.enabled && !state.audioBridgeEnabled && (
        <p className="voice-soft-note">板子当前离线，信令仍会下发并在上线后生效。</p>
      )}

      {state.audioBridgeMessage && (
        <div
          className={`message-banner voice-panel__message ${
            state.audioBridgeLastResult === "ok"
              ? "message-banner--success"
              : state.audioBridgeLastResult === "error"
                ? "message-banner--error"
                : "message-banner--muted"
          }`}
        >
          {state.audioBridgeMessage}
        </div>
      )}
    </div>
  );
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/dashboard/VoiceAssistantPanel.test.js
```
Expected: ALL PASS (6 tests).

- [ ] **Step 5: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/dashboard/VoiceAssistantPanel.jsx ref/src/dashboard/VoiceAssistantPanel.test.js
git commit -m "feat(dashboard): add lean VoiceAssistantPanel for collapsible region 4"
```

---

## Task 6: DashboardActionsMenu (PageShell actions slot)

The 3 low-frequency operations move into a menu attached to `<PageShell actions>`: 发送测试消息 / 复制桌面设备 ID / 解绑设备 (danger).

**Files:**
- Create: `ref/src/dashboard/DashboardActionsMenu.jsx`
- Create: `ref/src/dashboard/DashboardActionsMenu.test.js`
- Modify: `ref/src/styles.css` (add `.dashboard-actions-menu` rules)

- [ ] **Step 1: Write the failing test**

Create `ref/src/dashboard/DashboardActionsMenu.test.js`:

```javascript
/**
 * [Input] Read DashboardActionsMenu.jsx source.
 * [Output] Static Node coverage for the 3 menu items, the danger styling on 解绑, and the expected callback prop signature.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "DashboardActionsMenu.jsx"), "utf8");

test("DashboardActionsMenu exports a default React component", () => {
  assert.match(source, /export default function DashboardActionsMenu\s*\(/);
});

test("Menu renders the 3 items in the spec order", () => {
  const sendIdx = source.indexOf("发送测试消息");
  const copyIdx = source.indexOf("复制桌面设备 ID");
  const unbindIdx = source.indexOf("解绑设备");
  assert.ok(sendIdx !== -1, "expected 发送测试消息");
  assert.ok(copyIdx !== -1, "expected 复制桌面设备 ID");
  assert.ok(unbindIdx !== -1, "expected 解绑设备");
  assert.ok(sendIdx < copyIdx && copyIdx < unbindIdx, "items render in spec order");
});

test("解绑 is styled as danger", () => {
  assert.match(source, /dashboard-actions-menu__item--danger/);
});

test("Menu accepts onSendTest, onCopyDesktopId, onUnbind props", () => {
  for (const prop of ["onSendTest", "onCopyDesktopId", "onUnbind"]) {
    assert.match(source, new RegExp(`\\b${prop}\\b`), `expected prop ${prop}`);
  }
});

test("Menu hides itself when the trigger is clicked outside / Escape pressed", () => {
  // Implementation hook for click-outside; either listens to document mousedown or wraps in a Portal+overlay.
  assert.match(source, /onMouseDown|onClick.*setOpen|backdrop/);
});
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/dashboard/DashboardActionsMenu.test.js
```
Expected: ALL FAIL (file missing).

- [ ] **Step 3: Implement DashboardActionsMenu**

Create `ref/src/dashboard/DashboardActionsMenu.jsx`:

```jsx
/**
 * [Input] onSendTest / onCopyDesktopId / onUnbind callbacks plus disabled flags.
 * [Output] Three-item menu mounted into PageShell actions: 发送测试消息 / 复制桌面设备 ID / 解绑设备 (danger).
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React, { useEffect, useRef, useState } from "react";
import { MoreHorizontal, Send, Copy, Unlink } from "lucide-react";

export default function DashboardActionsMenu({ onSendTest, onCopyDesktopId, onUnbind }) {
  const [open, setOpen] = useState(false);
  const wrapRef = useRef(null);

  useEffect(() => {
    if (!open) return undefined;
    const handler = (event) => {
      if (wrapRef.current && !wrapRef.current.contains(event.target)) setOpen(false);
    };
    document.addEventListener("mousedown", handler);
    return () => document.removeEventListener("mousedown", handler);
  }, [open]);

  const dispatch = (cb) => () => {
    setOpen(false);
    cb?.();
  };

  return (
    <div className="dashboard-actions-menu" ref={wrapRef}>
      <button
        type="button"
        className="icon-btn dashboard-actions-menu__trigger"
        onClick={() => setOpen((prev) => !prev)}
        aria-haspopup="menu"
        aria-expanded={open}
        title="更多操作"
      >
        <MoreHorizontal size={16} />
      </button>
      {open && (
        <div className="dashboard-actions-menu__list" role="menu">
          <button
            type="button"
            className="dashboard-actions-menu__item"
            role="menuitem"
            onClick={dispatch(onSendTest)}
          >
            <Send size={14} />
            发送测试消息
          </button>
          <button
            type="button"
            className="dashboard-actions-menu__item"
            role="menuitem"
            onClick={dispatch(onCopyDesktopId)}
          >
            <Copy size={14} />
            复制桌面设备 ID
          </button>
          <button
            type="button"
            className="dashboard-actions-menu__item dashboard-actions-menu__item--danger"
            role="menuitem"
            onClick={dispatch(onUnbind)}
          >
            <Unlink size={14} />
            解绑设备
          </button>
        </div>
      )}
    </div>
  );
}
```

- [ ] **Step 4: Add CSS for the menu**

In `ref/src/styles.css`, append inside the `/* === dashboard ===` section:

```css
.dashboard-actions-menu {
  position: relative;
}

.dashboard-actions-menu__list {
  position: absolute;
  top: calc(100% + 6px);
  right: 0;
  min-width: 180px;
  padding: 6px;
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: var(--radius-md);
  box-shadow: var(--shadow-md);
  display: flex;
  flex-direction: column;
  gap: 2px;
  z-index: 100;
}

.dashboard-actions-menu__item {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 8px 10px;
  border: none;
  background: transparent;
  border-radius: var(--radius-sm);
  font-size: 13px;
  color: var(--text);
  text-align: left;
  cursor: pointer;
}

.dashboard-actions-menu__item:hover {
  background: var(--surface-muted);
}

.dashboard-actions-menu__item--danger {
  color: var(--danger);
}

.dashboard-actions-menu__item--danger:hover {
  background: var(--danger-soft);
}
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/dashboard/DashboardActionsMenu.test.js
```
Expected: ALL PASS (5 tests).

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/dashboard/DashboardActionsMenu.jsx ref/src/dashboard/DashboardActionsMenu.test.js ref/src/styles.css
git commit -m "feat(dashboard): add DashboardActionsMenu for PageShell actions"
```

---

## Task 7: Rewrite DeviceDashboard.jsx as the 4-section orchestrator

This is the integration task. We delete the page-hero / runtime-card / DesktopPetAssignmentPanel / VoiceAssistantPanel / BoardButtonConfigPanel / BoardButtonMap / AgentAppearancePickerModal / AgentAppearancePickerOption sub-functions from `DeviceDashboard.jsx`, delete the local polling effects (USB / online / appearance load / bridge profile load / detect_local_agents), and compose `<PageShell>` + 4 `<Card>`s pulling from `useDeviceContext()` and `useToast()`.

**Files:**
- Modify: `ref/src/DeviceDashboard.jsx` (the rewrite)
- Modify: `ref/src/styles.css` (only if dead rules need cleanup AND are referenced nowhere)
- Modify: `ref/src/.folder.md` (refresh the `DeviceDashboard.jsx` / `DeviceDashboard.test.js` row descriptions)

- [ ] **Step 1: Write the failing structural test (IA assertion)**

This test asserts the new 4-section IA. Open `ref/src/DeviceDashboard.test.js` and APPEND this block at the end of the file (do NOT delete existing tests yet — they will fail in Step 2 and we adapt them in Step 8):

```javascript
test("dashboard composes PageShell + 4 cards in the spec order (区 1 → 区 4)", () => {
  const source = readSource("DeviceDashboard.jsx");

  // Imports shell + dashboard children.
  assert.match(source, /from\s+"\.\/shell\/DeviceContext\.jsx"/);
  assert.match(source, /from\s+"\.\/shell\/ToastStack\.jsx"/);
  assert.match(source, /PageShell/);
  assert.match(source, /Card/);
  assert.match(source, /Card\.Collapsible/);
  assert.match(source, /DeviceStatusBar/);
  assert.match(source, /CurrentDisplayCard/);
  assert.match(source, /BoardButtonPanel/);
  assert.match(source, /VoiceAssistantPanel/);
  assert.match(source, /DashboardActionsMenu/);

  // The render returns a PageShell, in spec order.
  const idxStatusBar = source.indexOf("<DeviceStatusBar");
  const idxCurrent = source.indexOf("<CurrentDisplayCard");
  const idxButtons = source.indexOf("<BoardButtonPanel");
  const idxVoice = source.indexOf("<VoiceAssistantPanel");
  assert.ok(idxStatusBar !== -1 && idxCurrent !== -1 && idxButtons !== -1 && idxVoice !== -1);
  assert.ok(idxStatusBar < idxCurrent, "区 1 before 区 2");
  assert.ok(idxCurrent < idxButtons, "区 2 before 区 3");
  assert.ok(idxButtons < idxVoice, "区 3 before 区 4");
});

test("dashboard pulls device state from useDeviceContext (no local polling)", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.match(source, /useDeviceContext\(/);
  assert.match(source, /useToast\(/);
  // The old local polling effects are gone.
  assert.doesNotMatch(source, /invoke\("usb_get_status"\)/);
  assert.doesNotMatch(source, /invoke\("check_device_availability"\)/);
  assert.doesNotMatch(source, /invoke\("detect_local_agents"\)/);
  assert.doesNotMatch(source, /invoke\("load_bridge_profile"\)/);
  assert.doesNotMatch(source, /listAppearances\(\)/);
});

test("dashboard deletes the old runtime/Bridge card and inline panels", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.doesNotMatch(source, /function DesktopPetAssignmentPanel/);
  assert.doesNotMatch(source, /function VoiceAssistantPanel/);
  assert.doesNotMatch(source, /function BoardButtonConfigPanel/);
  assert.doesNotMatch(source, /function BoardButtonMap/);
  assert.doesNotMatch(source, /function AgentAppearancePickerModal/);
  assert.doesNotMatch(source, /dashboard-runtime-card/);
  assert.doesNotMatch(source, /运行状态/);
  assert.doesNotMatch(source, /desktop-pet-channel-card/);
});

test("dashboard places the actions menu inside PageShell actions slot", () => {
  const source = readSource("DeviceDashboard.jsx");

  assert.match(source, /actions=\{[\s\S]*?<DashboardActionsMenu/);
});

test("dashboard puts BoardButtonPanel inside a plain Card (always visible) and VoiceAssistantPanel inside Card.Collapsible", () => {
  const source = readSource("DeviceDashboard.jsx");

  const buttonsIdx = source.indexOf("<BoardButtonPanel");
  const cardBeforeButtons = source.lastIndexOf("<Card", buttonsIdx);
  const collapsibleBeforeButtons = source.lastIndexOf("<Card.Collapsible", buttonsIdx);
  assert.ok(cardBeforeButtons > collapsibleBeforeButtons, "BoardButtonPanel must live in a plain Card");

  const voiceIdx = source.indexOf("<VoiceAssistantPanel");
  const collapsibleBeforeVoice = source.lastIndexOf("<Card.Collapsible", voiceIdx);
  assert.ok(collapsibleBeforeVoice !== -1 && collapsibleBeforeVoice < voiceIdx, "VoiceAssistantPanel must live in Card.Collapsible");
});
```

- [ ] **Step 2: Run the new tests to verify they fail**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/DeviceDashboard.test.js
```
Expected: the 5 new tests at the bottom FAIL; many existing tests also fail because they assert on the old `DesktopPetAssignmentPanel` / `VoiceAssistantPanel` / `dashboard-runtime-card` markup that we're about to delete. Both kinds of failure are expected — Step 8 trims the obsolete legacy tests after the rewrite.

- [ ] **Step 3: Rewrite DeviceDashboard.jsx**

Replace the entire contents of `ref/src/DeviceDashboard.jsx` with this orchestrator. The shared constants (`BOARD_BUTTON_CONTROL_ROWS`, etc.) stay re-exported from this file so `BoardButtonPanel.jsx` can keep importing them.

```jsx
/**
 * [Input] Bound device, useDeviceContext for state, useToast for notices.
 * [Output] Device dashboard composed of PageShell + 4 cards: 状态条 / 当前展示 / 按钮配置 / 语音助手(折叠).
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useReducer, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import DeviceGuideModal from "./DeviceGuideModal.jsx";
import { DEVICE_GUIDE_SEEN_KEY } from "./lib/device-guide-content.js";
import PageShell from "./shell/PageShell.jsx";
import Card from "./shell/Card.jsx";
import { useDeviceContext } from "./shell/DeviceContext.jsx";
import { useToast } from "./shell/ToastStack.jsx";
import DeviceStatusBar from "./dashboard/DeviceStatusBar.jsx";
import CurrentDisplayCard from "./dashboard/CurrentDisplayCard.jsx";
import BoardButtonPanel from "./dashboard/BoardButtonPanel.jsx";
import VoiceAssistantPanel, { buildVoiceSummary } from "./dashboard/VoiceAssistantPanel.jsx";
import DashboardActionsMenu from "./dashboard/DashboardActionsMenu.jsx";

// ---------- Voice config storage + constants (re-exported for BoardButtonPanel) ----------

export const VOICE_CONFIG_STORAGE_KEY = "pet-manager.board-voice-config";
export const DEFAULT_VOICE_CONFIG = { enabled: false, trigger: "top_button.hold" };

export const DEFAULT_BUTTON_ACTIONS = {
  top_button: "voice_ptt",
  encoder_button: "system_reset",
  encoder_rotate: "system_page",
  screen_tap: "negative_screen_primary",
  screen_long_press: "negative_screen_secondary",
};

export const VOICE_BUTTON_OPTIONS = [
  { id: "top_button.hold", label: "顶部红色按钮", detail: "启用后按住说话；停止语音后恢复切屏 / 重启。", event: "voice.ptt.top_button.hold" },
  { id: "encoder_button.hold", label: "前方旋钮按压", detail: "启用后按住说话；旋钮旋转仍切屏。", event: "voice.ptt.encoder_button.hold" },
];

export const BUTTON_FUNCTION_OPTIONS = [
  { id: "voice_ptt", label: "语音按住说话", detail: "按住接入语音；同一时间只允许一个硬件按钮作为语音触发。" },
  { id: "system_page", label: "系统切页", detail: "保持 main / stats 页面切换，适合旋钮或顶部按钮。" },
  { id: "system_reset", label: "系统重置", detail: "保留长按重启或重置配网等板端默认能力。" },
  { id: "negative_screen_primary", label: "负一屏主操作", detail: "触发当前负一屏 widget 的主要 action，例如开始、确认、查看详情。" },
  { id: "negative_screen_secondary", label: "负一屏次操作", detail: "触发当前负一屏 widget 的次要 action，例如重置、更多、刷新。" },
  { id: "negative_screen_adjust", label: "负一屏调整", detail: "适合旋钮旋转，映射到调节时长、切换来源等连续动作。" },
  { id: "disabled", label: "不绑定", detail: "客户端不覆盖该按钮，板端继续使用组件包或系统默认配置。" },
];

export const BOARD_BUTTON_CONTROL_ROWS = [
  { id: "top_button", label: "顶部红色按钮", event: "button.primary.short_press / long_press", voiceTriggerId: "top_button.hold", defaultAction: "system_page", actionOptions: ["voice_ptt", "system_page", "negative_screen_primary", "negative_screen_secondary", "disabled"] },
  { id: "encoder_button", label: "前方旋钮按压", event: "button.encoder.hold", voiceTriggerId: "encoder_button.hold", defaultAction: "system_reset", actionOptions: ["voice_ptt", "system_reset", "negative_screen_primary", "negative_screen_secondary", "disabled"] },
  { id: "encoder_rotate", label: "前方旋钮旋转", event: "knob.rotate_cw / knob.rotate_ccw", defaultAction: "system_page", actionOptions: ["system_page", "negative_screen_adjust", "negative_screen_primary", "disabled"] },
  { id: "screen_tap", label: "负一屏屏幕点击", event: "screen.region.tap", defaultAction: "negative_screen_primary", actionOptions: ["negative_screen_primary", "negative_screen_secondary", "system_page", "disabled"] },
  { id: "screen_long_press", label: "负一屏屏幕长按", event: "screen.region.long_press", defaultAction: "negative_screen_secondary", actionOptions: ["negative_screen_secondary", "negative_screen_primary", "system_reset", "disabled"] },
];

export function actionOptionById(actionId) {
  return BUTTON_FUNCTION_OPTIONS.find((option) => option.id === actionId) || BUTTON_FUNCTION_OPTIONS[0];
}

function normalizeVoiceConfig(value = {}) {
  const triggerIds = new Set(VOICE_BUTTON_OPTIONS.map((o) => o.id));
  const trigger = triggerIds.has(value.trigger) ? value.trigger : DEFAULT_VOICE_CONFIG.trigger;
  const allowedActions = new Set(BUTTON_FUNCTION_OPTIONS.map((o) => o.id));
  const incoming = value.buttonActions && typeof value.buttonActions === "object" ? value.buttonActions : {};
  const buttonActions = BOARD_BUTTON_CONTROL_ROWS.reduce((next, row) => {
    next[row.id] = allowedActions.has(incoming[row.id]) ? incoming[row.id] : DEFAULT_BUTTON_ACTIONS[row.id] || row.defaultAction;
    return next;
  }, {});
  return { enabled: value.enabled === true, trigger, buttonActions };
}

function loadVoiceConfigFromStorage() {
  try {
    const raw = localStorage.getItem(VOICE_CONFIG_STORAGE_KEY);
    if (raw) return normalizeVoiceConfig(JSON.parse(raw));
  } catch {}
  return normalizeVoiceConfig({});
}

function saveVoiceConfigToStorage(next) {
  try {
    localStorage.setItem(VOICE_CONFIG_STORAGE_KEY, JSON.stringify(normalizeVoiceConfig(next)));
  } catch {}
}

// ---------- Voice-bus helpers (unchanged from previous file) ----------
const VOICE_BUS_URL = "http://127.0.0.1:8181";
async function fetchBusStatus(signal) {
  const resp = await fetch(`${VOICE_BUS_URL}/agent/status`, { signal });
  if (!resp.ok) throw new Error(`bus status http ${resp.status}`);
  const body = await resp.json();
  const list = Array.isArray(body?.adapters) ? body.adapters : Array.isArray(body?.agents) ? body.agents : [];
  return { ok: body?.ok !== false, agents: list };
}
async function fetchBusSessions(agentId, signal) {
  if (!agentId) return [];
  const resp = await fetch(`${VOICE_BUS_URL}/agent/sessions?agentId=${encodeURIComponent(agentId)}&limit=20`, { signal });
  if (!resp.ok) throw new Error(`bus sessions http ${resp.status}`);
  const body = await resp.json();
  return Array.isArray(body?.sessions) ? body.sessions : [];
}

// ---------- Voice reducer (lean — only what VoiceAssistantPanel needs) ----------
const VOICE_INITIAL_STATE = {
  busStatus: null,
  busSessions: null,
  busSessionId: "auto",
  voiceRuntime: null,
  audioBridgeEnabled: false,
  audioBridgePending: false,
  audioBridgeMessage: "",
  audioBridgeLastResult: null,
};

function voiceReducer(state, action) {
  switch (action.type) {
    case "set_bus_status": return { ...state, busStatus: action.value };
    case "set_bus_sessions": return { ...state, busSessions: action.value };
    case "set_bus_session_id": return { ...state, busSessionId: action.value || "auto" };
    case "set_voice_runtime": return { ...state, voiceRuntime: action.value };
    case "set_audio_bridge_pending": return { ...state, audioBridgePending: action.value };
    case "set_audio_bridge_state":
      return {
        ...state,
        audioBridgeEnabled: action.enabled,
        audioBridgePending: false,
        audioBridgeLastResult: action.ok ? "ok" : "error",
        audioBridgeMessage: action.message || "",
      };
    default: return state;
  }
}

// ---------- Component ----------

export default function DeviceDashboard({ binding, onUnbind }) {
  const { usb, deviceOnline, onlineBoardDeviceId, currentDisplay } = useDeviceContext();
  const { push } = useToast();

  const [voiceState, voiceDispatch] = useReducer(voiceReducer, VOICE_INITIAL_STATE);
  const [voiceConfig, setVoiceConfig] = useState(loadVoiceConfigFromStorage);
  const [voiceConfigDirty, setVoiceConfigDirty] = useState(false);
  const [voiceConfigOtaState, setVoiceConfigOtaState] = useState({ pending: false, tone: "", message: "" });
  const [guideOpen, setGuideOpen] = useState(false);

  // Auto-open the device-guide modal the first time the user lands here.
  useEffect(() => {
    if (!binding) return;
    try {
      if (!localStorage.getItem(DEVICE_GUIDE_SEEN_KEY)) setGuideOpen(true);
    } catch {}
  }, [binding]);

  const selectedAgentId = currentDisplay.agentId;

  // Restore last-picked voice session per agent.
  useEffect(() => {
    if (!selectedAgentId) return;
    try {
      const raw = localStorage.getItem(`pet-manager.voice-session.${selectedAgentId}`);
      voiceDispatch({ type: "set_bus_session_id", value: raw || "auto" });
    } catch {
      voiceDispatch({ type: "set_bus_session_id", value: "auto" });
    }
  }, [selectedAgentId]);

  // Poll voice-bus status.
  useEffect(() => {
    let cancelled = false;
    const ctl = new AbortController();
    const run = () => {
      fetchBusStatus(ctl.signal)
        .then((body) => { if (!cancelled) voiceDispatch({ type: "set_bus_status", value: body }); })
        .catch(() => { if (!cancelled) voiceDispatch({ type: "set_bus_status", value: { ok: false, agents: [] } }); });
    };
    run();
    const id = setInterval(run, 5000);
    return () => { cancelled = true; ctl.abort(); clearInterval(id); };
  }, []);

  // Poll voice-runtime.
  useEffect(() => {
    let cancelled = false;
    const run = () => {
      invoke("ensure_voice_runtime")
        .then((res) => {
          if (cancelled) return;
          voiceDispatch({
            type: "set_voice_runtime",
            value: {
              mode: res?.mode || null,
              message: res?.message || "",
              running: !!res?.running,
              agentId: res?.selectedAgentId || res?.profile?.selectedAgentId || "",
            },
          });
        })
        .catch((err) => {
          if (cancelled) return;
          voiceDispatch({ type: "set_voice_runtime", value: { mode: "error", message: String(err), running: false, agentId: "" } });
        });
    };
    run();
    const id = setInterval(run, 5000);
    return () => { cancelled = true; clearInterval(id); };
  }, [selectedAgentId]);

  // Refetch sessions for the current agent.
  useEffect(() => {
    if (!selectedAgentId) { voiceDispatch({ type: "set_bus_sessions", value: [] }); return undefined; }
    let cancelled = false;
    const ctl = new AbortController();
    fetchBusSessions(selectedAgentId, ctl.signal)
      .then((sessions) => { if (!cancelled) voiceDispatch({ type: "set_bus_sessions", value: sessions }); })
      .catch(() => { if (!cancelled) voiceDispatch({ type: "set_bus_sessions", value: [] }); });
    return () => { cancelled = true; ctl.abort(); };
  }, [selectedAgentId]);

  // ---------- Voice config update + OTA dispatch ----------
  const updateVoiceConfig = useCallback((patch) => {
    setVoiceConfig((prev) => {
      const next = normalizeVoiceConfig({ ...prev, ...patch });
      saveVoiceConfigToStorage(next);
      setVoiceConfigDirty(true);
      setVoiceConfigOtaState({ pending: false, tone: "warning", message: "已保存到客户端；按钮配置需要通过 USB OTA 下发到板端后才会生效。" });
      return next;
    });
  }, []);

  const applyVoiceConfigOverUsb = useCallback(async () => {
    if (!usb.connected) {
      setVoiceConfigOtaState({ pending: false, tone: "warning", message: "需要先通过 USB 连接设备，才能把按钮配置 OTA 到板端。" });
      return;
    }
    const targetBoardDeviceId = onlineBoardDeviceId || usb.boardDeviceId || binding.boardDeviceId;
    if (!targetBoardDeviceId) {
      setVoiceConfigOtaState({ pending: false, tone: "error", message: "未找到可用的板子 ID，请先完成设备绑定。" });
      return;
    }
    setVoiceConfigOtaState({ pending: true, tone: "", message: "正在通过 USB / MQTT 下发到板端..." });
    try {
      const res = await invoke("audio_bridge_signal", {
        boardDeviceId: targetBoardDeviceId,
        action: voiceConfig.enabled ? "start" : "stop",
        voiceButton: voiceConfig.trigger,
      });
      const transports = [res?.usbSent ? "USB" : "", res?.mqttSent ? "MQTT" : ""].filter(Boolean).join(" / ");
      setVoiceConfigDirty(false);
      setVoiceConfigOtaState({ pending: false, tone: "success", message: `语音按钮配置已通过 ${transports || "USB / MQTT"} 下发到板端。` });
    } catch (err) {
      setVoiceConfigOtaState({ pending: false, tone: "error", message: `语音按钮配置下发失败: ${err}` });
    }
  }, [binding.boardDeviceId, onlineBoardDeviceId, usb.boardDeviceId, usb.connected, voiceConfig.enabled, voiceConfig.trigger]);

  // ---------- Audio bridge toggle ----------
  const toggleAudioBridge = useCallback(async (action) => {
    const requestedEnabled = action === "start";
    const targetBoardDeviceId = onlineBoardDeviceId || binding.boardDeviceId;
    voiceDispatch({ type: "set_audio_bridge_pending", value: true });
    try {
      if (!targetBoardDeviceId) throw new Error("未找到可用的板子 ID，请先完成设备绑定。");
      if (action === "start") {
        const bridgeRuntime = await invoke("ensure_bridge_runtime");
        if (bridgeRuntime?.running === false) throw new Error(bridgeRuntime?.message || "本地 Bridge 未启动，无法下发板子音频信令。");
        const voiceRuntime = await invoke("ensure_voice_runtime");
        if (!voiceRuntime?.running) throw new Error(voiceRuntime?.message || "voice-service 未启动，无法接入板子音频。");
      }
      const res = await invoke("audio_bridge_signal", {
        boardDeviceId: targetBoardDeviceId,
        action,
        voiceButton: voiceConfig.trigger,
      });
      const transports = [res?.usbSent ? "USB" : "", res?.mqttSent ? "MQTT" : ""].filter(Boolean).join(" / ");
      voiceDispatch({ type: "set_audio_bridge_state", enabled: requestedEnabled, ok: true, message: `已通过 ${transports || "USB / MQTT"} 下发到板端` });
    } catch (err) {
      voiceDispatch({ type: "set_audio_bridge_state", enabled: !requestedEnabled, ok: false, message: `${action === "start" ? "启动" : "关闭"}板子音频失败: ${err}` });
    }
  }, [binding.boardDeviceId, onlineBoardDeviceId, voiceConfig.trigger]);

  // ---------- Action-menu callbacks ----------
  const onSendTest = useCallback(() => {
    const sendPromise = usb.connected
      ? invoke("usb_send_speech", { text: "hello from pet-manager" }).then(() => ({ ok: true })).catch((err) => ({ ok: false, error: String(err) }))
      : invoke("send_test_message", { desktopDeviceId: binding.desktopDeviceId, namespace: null, text: null });
    sendPromise.then((res) => {
      push(res.ok
        ? { tone: "success", title: "测试消息已发送" }
        : { tone: "error", title: "测试消息发送失败", message: res.error });
    });
  }, [binding.desktopDeviceId, push, usb.connected]);

  const onCopyDesktopId = useCallback(() => {
    try {
      navigator.clipboard?.writeText(binding.desktopDeviceId || "");
      push({ tone: "success", title: "已复制桌面设备 ID" });
    } catch {
      push({ tone: "error", title: "复制失败" });
    }
  }, [binding.desktopDeviceId, push]);

  const onUnbindClick = useCallback(() => {
    invoke("remove_device_binding", { boardDeviceId: binding.boardDeviceId })
      .then(() => onUnbind?.())
      .catch((err) => push({ tone: "error", title: "解绑失败", message: String(err) }));
  }, [binding.boardDeviceId, onUnbind, push]);

  const selectedVoiceTrigger = VOICE_BUTTON_OPTIONS.find((o) => o.id === voiceConfig.trigger) || VOICE_BUTTON_OPTIONS[0];

  return (
    <PageShell
      title="桌搭控制台"
      help={() => setGuideOpen(true)}
      actions={
        <DashboardActionsMenu
          onSendTest={onSendTest}
          onCopyDesktopId={onCopyDesktopId}
          onUnbind={onUnbindClick}
        />
      }
    >
      <Card>
        <DeviceStatusBar />
      </Card>

      <Card title="当前展示">
        <CurrentDisplayCard />
      </Card>

      <Card title="按钮配置" subtitle="按键当前的作用，可直接编辑">
        <BoardButtonPanel
          voiceConfig={voiceConfig}
          buttonActions={voiceConfig.buttonActions}
          voiceConfigDirty={voiceConfigDirty}
          voiceConfigOtaState={voiceConfigOtaState}
          usbConnected={Boolean(usb.connected)}
          selectedTrigger={selectedVoiceTrigger}
          onVoiceConfigChange={updateVoiceConfig}
          onApplyVoiceConfig={applyVoiceConfigOverUsb}
        />
      </Card>

      <Card.Collapsible
        title="语音助手"
        summary={buildVoiceSummary(voiceConfig, selectedVoiceTrigger)}
      >
        <VoiceAssistantPanel
          state={{
            ...voiceState,
            selectedAgentId,
            deviceOnline,
          }}
          dispatch={voiceDispatch}
          toggleAudioBridge={toggleAudioBridge}
          voiceConfig={voiceConfig}
          selectedTrigger={selectedVoiceTrigger}
          onVoiceConfigChange={updateVoiceConfig}
        />
      </Card.Collapsible>

      <DeviceGuideModal isOpen={guideOpen} onClose={() => setGuideOpen(false)} />
    </PageShell>
  );
}
```

- [ ] **Step 4: Run the new IA tests to verify they pass**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/DeviceDashboard.test.js
```
Expected: the 5 new IA tests at the BOTTOM of the file PASS. Pre-existing tests above them still FAIL (they assert on deleted markup). Step 8 trims those.

- [ ] **Step 5: Update `ref/src/.folder.md` rows for DeviceDashboard.jsx / DeviceDashboard.test.js**

Open `ref/src/.folder.md`. Replace the existing `DeviceDashboard.jsx` row's Function column with:

```markdown
| `DeviceDashboard.jsx` | `component` | 4-section device dashboard composed on the shared shell: PageShell + DashboardActionsMenu (actions slot) + DeviceStatusBar (区 1) + CurrentDisplayCard (区 2) + BoardButtonPanel (区 3, always visible) + Card.Collapsible<VoiceAssistantPanel> (区 4). Pulls all device/USB/online/appearance/agent state from `useDeviceContext()` and surfaces success/error/progress notices through `useToast()`. Owns voice-config storage, voice-bus polling, and audio-bridge toggling; exports the BOARD_BUTTON_CONTROL_ROWS / BUTTON_FUNCTION_OPTIONS constants for BoardButtonPanel to import. |
```

Replace the existing `DeviceDashboard.test.js` row with:

```markdown
| `DeviceDashboard.test.js` | `test` | Static Node coverage that the dashboard composes PageShell + 4 cards in spec order (状态条 / 当前展示 / 按钮配置 / 语音助手折叠), pulls state from useDeviceContext (no local polling), removes the old runtime/desktop-pet-channel panels, places DashboardActionsMenu in PageShell actions, places BoardButtonPanel inside a plain Card and VoiceAssistantPanel inside Card.Collapsible. |
```

- [ ] **Step 6: Run a sanity dev build**

```bash
cd /path/to/claw-pet-manager/ref
npm run build:web 2>&1 | tail -30
```
Expected: build SUCCEEDS. If it fails on a missing import / dead reference, fix the missing identifier — do NOT silence the error.

- [ ] **Step 7: Commit the orchestrator rewrite**

```bash
cd /path/to/claw-pet-manager
git add ref/src/DeviceDashboard.jsx ref/src/DeviceDashboard.test.js ref/src/.folder.md
git commit -m "refactor(dashboard): rewrite DeviceDashboard as PageShell + 4 cards"
```

- [ ] **Step 8: Trim obsolete assertions in DeviceDashboard.test.js**

Open `ref/src/DeviceDashboard.test.js`. The existing test cases reference markup that no longer exists in the rewritten dashboard (`DesktopPetAssignmentPanel`, `dashboard-runtime-card`, `desktop-pet-channel-card`, `<h4>设备展示配置</h4>`, `<h4>运行状态</h4>`, the inline `VoiceAssistantPanel` / `BoardButtonConfigPanel` / `BoardButtonMap` function definitions, `function AgentAppearancePickerModal`).

Behavior tests that MUST be preserved (port the selectors to the new components):

1. **"channel switch confirmation owns its modal footer spacing"** — keep entirely. The modal file did not move; selectors still match.
2. **"confirming a channel switch persists the followed channel immediately"** — port: the behavior now lives in `CurrentDisplayCard.jsx`'s `confirmPendingChannel`. Change `readSource("DeviceDashboard.jsx")` to `readSource("dashboard/CurrentDisplayCard.jsx")` and update regex to:
   ```javascript
   assert.match(source, /shouldConfirmChannelSwitch\(/);
   assert.match(source, /confirmPendingChannel/);
   assert.match(source, /applyDesktopPet\(/);
   ```
3. **"voice button configuration exposes direct per-button settings before OTA"** — port to `dashboard/BoardButtonPanel.jsx`. Drop assertions that reference `function BoardButtonConfigPanel` / `function VoiceAssistantPanel` (no longer in `DeviceDashboard.jsx`). Keep assertions on `voice-button-action-list`, `voice-button-action-select`, `BOARD_BUTTON_CONTROL_ROWS.map`, `BUTTON_FUNCTION_OPTIONS.filter`, `通过 USB OTA 下发按钮配置`, `需 USB OTA 生效`.
4. **"button configuration shows a board button map with current assignments"** — port to `dashboard/BoardButtonPanel.jsx`. Update the assertion for the action-label upgrade: replace `/board-button-map__sync-strip/` (the sync strip is deleted) with `/board-button-panel__callout-label/` (the per-button action label).
5. **"board audio enable starts local runtimes and targets the active board"** — keep `targetBoardDeviceId` + `ensure_bridge_runtime` + `ensure_voice_runtime` assertions reading from the new orchestrator file (these now live in `DeviceDashboard.jsx`'s `toggleAudioBridge`). Update the `VoiceAssistantPanel` slice extraction to read from `dashboard/VoiceAssistantPanel.jsx` instead.
6. **"board voice button config is marked for USB OTA and sent to the board runtime"** — split: keep the Rust-side assertions on `src-tauri/src/lib.rs`; move the JS-side assertions about `voiceButton: voiceConfig.trigger` and `VOICE_CONFIG_STORAGE_KEY` to read from `DeviceDashboard.jsx` (still defined there); move the panel-side assertions (`按钮功能`, `通过 USB OTA 下发按钮配置`, `需 USB OTA 生效`) to read from `dashboard/BoardButtonPanel.jsx`.

DELETE these tests entirely (their assertions are about IA that the rewrite eliminates):

- **"dashboard configures one device display channel and one appearance"** — replaced by Task 3 + Task 7 IA tests.
- **"device display config is the single visible agent configuration surface"** — the `<h4>设备展示配置</h4>` heading no longer exists.
- **"bound device and runtime status use compact dashboard density"** — `dashboard-runtime-card` is deleted; `DeviceStatusBar.test.js` covers the replacement.
- **"voice assistant settings use coordinated dashboard styling"** — the `voice-panel__body` shell is replaced by `Card.Collapsible`. `VoiceAssistantPanel.test.js` covers the new shape.
- **"voice assistant keeps session and audio controls flat"** — same reason; controls are flat in the new panel by construction (assert in `VoiceAssistantPanel.test.js`).
- **"voice assistant sits below device display configuration"** — replaced by the IA test in Task 7 Step 1 (`区 2 before 区 4`).

After editing, run:

```bash
cd /path/to/claw-pet-manager/ref
node --test src/DeviceDashboard.test.js
```
Expected: ALL PASS.

- [ ] **Step 9: Commit the test trim**

```bash
cd /path/to/claw-pet-manager
git add ref/src/DeviceDashboard.test.js
git commit -m "test(dashboard): port behavioral assertions to new 4-section IA"
```

---

## Task 8: Cross-cutting smoke + project test run + manual verify

**Files:**
- No new files. This is verification + commit-glue.

- [ ] **Step 1: Full dashboard subfolder test run**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/dashboard/*.test.js
```
Expected: ALL PASS (5 test files).

- [ ] **Step 2: Full project test run**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/*.test.js src/lib/*.test.js src/shell/*.test.js src/dashboard/*.test.js
```
Expected: ALL PASS. If `ProductExperience.test.js` fails on text it expected to find inside the old `DeviceDashboard.jsx` (e.g. `运行状态`), update the assertion to read from `dashboard/DeviceStatusBar.jsx` or simply drop the stale assertion.

- [ ] **Step 3: Manual smoke test**

```bash
cd /path/to/claw-pet-manager/ref
npm run dev:web
```
Open `http://localhost:4173`, bind a device (or load a saved binding), and verify:

| Check | Pass condition |
|---|---|
| 4 sections render in order | 状态条 → 当前展示 → 按钮配置 → 语音助手（折叠头） |
| 区 1 状态 chip | USB 直连 / 在线 / 离线 三态根据 USB+WiFi 状态切换 |
| 区 2 渠道下拉 | 选已有 formosa 的渠道 → 弹 ChannelSwitchConfirmModal |
| 区 2 渠道下拉 (无 formosa 渠道) | 不弹 modal，沿用当前 formosa |
| 区 2「更换 ▾」 | 打开 picker；未连 USB 时 apply 按钮 disabled 并显示 hint |
| 区 3 SVG callouts | 每个按钮旁边显示当前 action 中文 label（不只 voice_ptt 高亮） |
| 区 3 hover SVG button | 下方对应行 highlight；hover 行 → SVG callout highlight |
| 区 3 USB OTA 按钮 | 未连 USB 时 disabled + tooltip 提示 |
| 区 4 默认折叠 | 头部显示 `已开启 · top_button.hold` 或 `未开启` |
| 区 4 展开后 | 仅有 开关 / 续接 / 启动收听 三组控件（无按钮配置） |
| 右上 ⋯ 菜单 | 3 项 (发送测试消息 / 复制桌面设备 ID / 解绑设备 danger 红字)；解绑成功后回到 setup 页 |
| ContextRail 同步 | 切换渠道/形象后，左侧 sidebar ContextRail 的形象行同步更新 |
| Toast | 同步成功 / 失败 / USB-required 警告 都通过 ToastStack 展示 |

Ctrl+C the dev server when done. If any item fails, fix and re-test before committing.

- [ ] **Step 4: Final commit (if any fixes from smoke test)**

```bash
cd /path/to/claw-pet-manager
git status
# if changes exist:
git add -p
git commit -m "fix(dashboard): smoke-test fixes for 4-section IA"
```

If no changes, skip this step.

---

## Definition of Done for Plan 2

- [ ] `ref/src/dashboard/` contains 5 components + 5 tests + `.folder.md`
- [ ] `ref/src/DeviceDashboard.jsx` is the orchestrator: composes `<PageShell>` + `<DashboardActionsMenu>` + 4 `<Card>` regions in spec order, pulls all device state from `useDeviceContext()`, surfaces notices through `useToast()`
- [ ] All inline polling effects (USB / online / detect_local_agents / load_bridge_profile / listAppearances) are deleted from `DeviceDashboard.jsx`
- [ ] `DesktopPetAssignmentPanel` / `AgentAppearancePickerModal` / `BoardButtonConfigPanel` / `VoiceAssistantPanel` / `BoardButtonMap` are deleted from `DeviceDashboard.jsx` (split into the dashboard/ folder)
- [ ] BoardButtonPanel's SVG renders each callout's current action label; hover on an SVG button highlights the matching editor row and vice-versa
- [ ] CurrentDisplayCard exposes TWO independent inputs (渠道 `<select>` + 「更换 ▾」 button); the 4 spec-table routing branches are wired (`shouldConfirmChannelSwitch` + USB-required gate)
- [ ] VoiceAssistantPanel no longer embeds button config; lives inside `<Card.Collapsible>` with summary `已开启 · <trigger>` / `未开启`
- [ ] All `src/dashboard/*.test.js` pass via `node --test`
- [ ] All `src/*.test.js src/lib/*.test.js src/shell/*.test.js` pass (no regressions on neighbor pages or Plan 1 shell)
- [ ] `npm run build:web` succeeds; `npm run dev:web` boots; smoke test checklist passes
- [ ] No file under `ref/src/shell/` was modified (Plan 1 API is honored as frozen)

---

## Self-Review (run by the writer, not by an agent)

**Spec coverage:**
- Spec § "Plan 2: Device Dashboard" IA (4 cards + actions menu) → Task 7 IA test + Task 2/3/4/5/6 ✓
- Spec § "渠道与形象的独立切换" 4-row table → Task 3 test cases (rows 1+2 via `shouldConfirmChannelSwitch`, row 3 via map fallback to current appearance, row 4 via USB-required gate on the picker) + Task 3 implementation ✓
- Spec § "按钮配置 SVG 的升级" — callout labels per button, hover sync, voice_ptt distinct chip → Task 4 test + implementation ✓
- Spec § "按钮配置 always visible, 不折叠" → Task 7 IA test "BoardButtonPanel inside plain Card" ✓
- Spec § "已删除的元素" — 运行状态卡 / 设备展示配置卡 / DesktopPetAssignmentPanel / VoiceAssistantPanel split → Task 7 deletion assertions ✓
- Spec § "Must consume useDeviceContext / useToast" → Task 2/3/4/5/6/7 imports + assertions on absence of `usb_get_status` / `listAppearances` etc. ✓
- Spec § "Must NOT modify ref/src/shell/" → enforced by file list in File Structure and Definition of Done ✓
- Spec § "保留 message-banner for in-context errors (appearance load)" → CurrentDisplayCard passes errors to toast, but `agent-appearance-picker-modal__notice` keeps its inline banner; future appearance-load-error banner is already preserved by `appearanceState.error` consumers when re-introduced ✓
- Spec § "Out of Scope: no Rust changes" → All Tauri commands consumed via existing `invoke(...)`; no command names changed ✓

**Placeholder scan:** None. Every step includes exact paths, exact code, exact commands, exact commit messages. The one "if needed" item (Task 8 Step 4) explicitly says "skip if no changes".

**Type consistency cross-check:**
- `applyDesktopPet(agentId, appearance, options)` — defined in Plan 1's `DeviceContext.jsx`; consumed in CurrentDisplayCard (Task 3) with the same signature.
- `useToast().push({ tone, title, message?, action?, ttl? })` — defined in Plan 1's `ToastStack.jsx`; consumed in CurrentDisplayCard / DeviceDashboard (Task 3, 7) with the same shape.
- `useDeviceContext()` fields used in Plan 2: `binding`, `usb`, `deviceOnline`, `onlineBoardDeviceId`, `appearances`, `agentAppearanceMap`, `agentOptions`, `currentDisplay`, `applyDesktopPet` — all in Plan 1's documented context shape.
- `buildVoiceSummary(voiceConfig, selectedTrigger)` — defined and exported in Task 5; consumed in Task 7 by the orchestrator.
- `BoardButtonPanel` props (`voiceConfig`, `buttonActions`, `voiceConfigDirty`, `voiceConfigOtaState`, `usbConnected`, `selectedTrigger`, `onVoiceConfigChange`, `onApplyVoiceConfig`) — match between Task 4 implementation and Task 7 caller.
- `DashboardActionsMenu` props (`onSendTest`, `onCopyDesktopId`, `onUnbind`) — match between Task 6 implementation and Task 7 caller.
- Constants `BOARD_BUTTON_CONTROL_ROWS` / `BUTTON_FUNCTION_OPTIONS` / `DEFAULT_BUTTON_ACTIONS` / `DEFAULT_VOICE_CONFIG` / `actionOptionById` — exported from `DeviceDashboard.jsx` (Task 7), imported by `BoardButtonPanel.jsx` (Task 4). Verified the export list in Task 7 Step 3 covers every name `BoardButtonPanel` imports.

**Known fragile areas:**

1. **Cross-file constant import** — `BoardButtonPanel.jsx` imports `BOARD_BUTTON_CONTROL_ROWS` etc. from `../DeviceDashboard.jsx`. Order of writing matters: Task 4 implements `BoardButtonPanel` before Task 7 rewrites `DeviceDashboard.jsx`. The existing pre-rewrite `DeviceDashboard.jsx` already exports nothing — but the constants are top-level `const` declarations. The Task 4 import will fail until those constants are explicitly `export`-prefixed. Mitigation: Task 4 Step 6 will COMMIT a broken import that Task 7 fixes; in practice, running `node --test src/dashboard/BoardButtonPanel.test.js` (Task 4 Step 5) uses static source analysis on `BoardButtonPanel.jsx`, NOT runtime imports, so the test passes regardless. The runtime import only matters for `npm run build:web` in Task 7 Step 6 — by then the orchestrator rewrite has added the `export` keywords.

2. **`ProductExperience.test.js` regression** — that file does cross-cut assertions on App.jsx + DeviceDashboard.jsx behavior. Likely failures are stale class names (e.g. `dashboard-runtime-card`). Task 8 Step 2 catches them; resolution is to drop the stale assertions or repoint them at the new components. The plan does not enumerate them ahead of time because the exact assertions depend on the file at execution time, but Step 2 makes them visible.

3. **Plan 1 shell freeze** — if Task 3 or Task 7 needs a `useDeviceContext` field not in the Plan 1 contract (e.g. `boardDeviceConnected` derived flag), STOP and file a shell amendment PR per the spec's "shell amendment" rule rather than reaching into the shell file. The current plan only uses documented fields (`binding`, `usb`, `deviceOnline`, `onlineBoardDeviceId`, `appearances`, `agentAppearanceMap`, `agentOptions`, `currentDisplay`, `applyDesktopPet`) — verified against Plan 1 Task 5's Provider context shape.

---
