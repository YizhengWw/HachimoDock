# Plan 3: Appearance Gallery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate `ref/src/AppearanceGallery.jsx` onto the Plan 1 shared shell. The gallery becomes a focused “browse + one-tap apply” surface: 3 creation buttons collapse into a `SplitButton` inside `<PageShell>` actions, every card shows a “使用中” badge when it matches the currently displayed appearance, the inline `sync-notice` floating banner is replaced by `useToast()`, and all local polling / agent-config state is read from `useDeviceContext()`. `AppearanceChannelModal` is preserved (visually reworked, logic unchanged) so users can still change channels from the gallery.

**Architecture:** The page becomes a thin presentational shell on top of `useDeviceContext()`. Rendering tree: `<PageShell title="形象画廊" actions={[<Button>刷新</Button>, <SplitButton/>]}>` → optional `<Card>` wrapping `<RunningTaskCard/>` (only when a generation task is in flight) → existing `.appearance-grid` with renamed `<AppearanceCard/>` → preserved `<CodexImportModal/>`, `<CommunityImportModal/>`, `<AppearanceChannelModal/>`, `<ChannelSwitchConfirmModal/>` portals. The Codex/community import modals retain their existing implementations untouched — only the entrypoints change. `SplitButton` is a tiny local helper inside `AppearanceGallery.jsx` (NOT in `ref/src/shell/` — shell is frozen).

**Tech Stack:** React 18, lucide-react (`RefreshCw`, `Sparkles`, `Download`, `Globe`, `ChevronDown`, `Check`, `UploadCloud`, `Loader`, `CheckCircle`, `CheckCircle2`, `X`, `AlertCircle`, `Code`, `Terminal`, `Zap`), Plan 1 shell (`PageShell`, `Card`, `useDeviceContext`, `useToast`), vanilla CSS (extending the existing `.appearance-*` and `.gallery-*` rules in `styles.css`), `node:test` for tests (static source analysis via `readFileSync` + regex assertions, matching the established `AppearanceGallery.test.js` pattern).

**Spec reference:** `docs/superpowers/specs/2026-05-26-pet-manager-layout-redesign-design.md` — sections "Plan 3: Appearance Gallery" and "关键决定" under that heading, plus "Out of Scope".

**Plan 1 dependency:** This plan MUST run after Plan 1 has landed. The shell components and `DeviceContextProvider` are consumed unchanged — **no file under `ref/src/shell/` may be modified by this plan**. If a shell gap surfaces during implementation, stop and request a shell amendment instead of editing shell files.

---

## File Structure

**New files:** none (`SplitButton` lives inside `AppearanceGallery.jsx` as a local helper).

**Modified files:**

| Path | What changes |
|------|--------------|
| `ref/src/AppearanceGallery.jsx` | Rewrite the default-exported `AppearanceGallery` component. Drop local USB / device-online useEffects, drop local `agentOptions` / `agentAppearanceMap` / `enabledAgents` state, drop the inline-style `sync-notice` block, drop the `usbConnected` / `usbBoardDeviceId` / `deviceOnline` / `onlineBoardDeviceId` state. Read all those from `useDeviceContext()`. Push success/warning/error toasts via `useToast().push()`. Replace `page-toolbar` + `page-hero` + `gallery-actions` with `<PageShell title="形象画廊" subtitle="..." actions={[refreshButton, <SplitButton/>]}>`. Wrap `RunningTaskCard` in `<Card>` and only render when `taskRunning`. Rename `CustomCard` → `AppearanceCard`, add `isActive` prop driving a “使用中” badge + `is-active` border, rename `onSelectChannel` → `onSetAsDesktopPet`. Add a local `SplitButton` helper. Keep `AppearanceChannelModal`, `CodexImportModal`, `CommunityImportModal`, `CodexPetRow`, `RunningTaskCard` and `configuredAgentIdsForAppearance` exactly as today, only updating `AppearanceChannelModal`’s outer chrome to use shell `<Card>` styling tokens. |
| `ref/src/AppearanceGallery.test.js` | Port behavioral assertions onto the new structure. Drop assertions tied to deleted code paths (local `deviceOnline` state, `usbConnected` polling, `sync-notice` inline style, `gallery-actions` div, `usbBoardDeviceId`). Add new assertions for: `PageShell` usage, `SplitButton` with the 3 creation labels, `RunningTaskCard` only renders inside the conditional `Card`, `isActive` prop driving “使用中”, `useDeviceContext()` is the source for USB/online/`agentAppearanceMap`/`enabledAgents`/`agentOptions`/`currentDisplay`, `useToast` replaces `sync-notice`, `AppearanceChannelModal` is still present and uses `applyDesktopPet` from context. Preserve the behavioral assertions that still apply (channel-selection step before desktop-pet assignment, `ChannelSwitchConfirmModal` still rendered, `configuredAgentIdsForAppearance` still used, Codex/community import labels still in tree). |
| `ref/src/styles.css` | Extend the gallery section: add `.appearance-card.is-active` styles (subtle accent border + bg), `.appearance-card__badge--active` chip styles, `.split-button` + `.split-button__menu` + `.split-button__item` rules, `.appearance-gallery__current-task` wrapper if needed. Keep all existing `.appearance-*` and `.gallery-*` rules — the `.gallery-actions` block becomes dead after Plan 3 but is intentionally NOT deleted here (spec § "Out of Scope" defers CSS GC). |
| `ref/src/.folder.md` | Update the `AppearanceGallery.jsx` row’s Function blurb and the `AppearanceGallery.test.js` row’s Function blurb to reflect the shell migration and the SplitButton / “使用中” badge / toast wiring. Sync the Architecture summary to mention the shell migration. |

**No changes** (out of scope for Plan 3):
- Any file under `ref/src/shell/` (shell is frozen after Plan 1).
- `ref/src/App.jsx` (Plan 1 already wires providers and `ContextRail`).
- `ref/src/DeviceDashboard.jsx`, `ref/src/ComponentCenter.jsx` (Plans 2 and 4).
- Any Tauri Rust source under `src-tauri/`.
- Any `ref/src/lib/*.js` file (we only **consume** `agent-appearance-config.js`, `desktop-pet-assignment.js`, `device-availability.js`, `appearance-store.js`, `builtin-appearances.js`, `generation-task.js`, `codex-pets-client.js`, `codex-community-import.js`, `appearance-preview.js`).
- `ref/src/AppearancePreview.jsx`, `ref/src/ChannelSwitchConfirmModal.jsx`, `ref/src/AppearanceDetail.jsx`, `ref/src/CustomAvatarWizard.jsx`, `ref/src/HelpTooltip.jsx`, `ref/src/CommunityImportHelp.jsx`.

---

## Conventions used in this plan

- **All commands assume `cd /path/to/claw-pet-manager/ref`** unless otherwise noted.
- **Test runner**: `node --test src/AppearanceGallery.test.js` (the project has no `test` npm script — invoke `node --test` directly).
- **Test style**: static source analysis via `readFileSync` + `assert.match` / `assert.doesNotMatch`, identical to the existing `AppearanceGallery.test.js`. Do NOT introduce jest, React Testing Library, jsdom, or any new dependency.
- **Commit style**: conventional, Chinese OK, no `--no-verify`. Pattern: `feat(gallery): <one-line summary>`. The final folder-doc commit uses `docs(gallery): ...`.
- **No new shell modifications**: if you find a missing shell prop, STOP and raise a shell amendment instead of editing shell files. This plan assumes `useDeviceContext()` exposes (at minimum): `usb`, `deviceOnline`, `deviceConnected`, `appearances`, `agentAppearanceMap`, `enabledAgents`, `agentOptions`, `currentDisplay`, `applyDesktopPet`, `refresh`, `onlineBoardDeviceId`, `binding` — all per Plan 1 Task 5.
- **Tests-first**: every behavioral change starts with a failing static-source assertion, then implementation, then a passing run, then commit.

---

## Task 1: Inventory existing test assertions and pin the deletions

This is a planning task — no source edits. The goal is to map every existing `AppearanceGallery.test.js` assertion to one of: KEEP / DROP / REWRITE. Subsequent tasks then make changes in coherent chunks.

**Files:**
- Read: `ref/src/AppearanceGallery.test.js`
- Read: `ref/src/AppearanceGallery.jsx`
- Read: `ref/src/shell/DeviceContext.jsx` (to confirm context API surface from Plan 1)
- Read: `ref/src/shell/ToastStack.jsx` (to confirm `useToast` signature from Plan 1)
- Read: `ref/src/shell/PageShell.jsx` and `ref/src/shell/Card.jsx` (to confirm props)

- [ ] **Step 1: Verify the Plan 1 shell exists**

```bash
cd /path/to/claw-pet-manager/ref
ls src/shell/
```
Expected output includes: `PageShell.jsx`, `Card.jsx`, `ToastStack.jsx`, `DeviceContext.jsx`, `ContextRail.jsx`, `.folder.md`. If any file is missing, STOP — Plan 1 has not landed yet, do not start Plan 3.

- [ ] **Step 2: Confirm `useDeviceContext` exposes the fields this plan needs**

```bash
cd /path/to/claw-pet-manager/ref
grep -E "binding|usb|deviceOnline|deviceConnected|appearances|agentAppearanceMap|enabledAgents|agentOptions|currentDisplay|applyDesktopPet|refresh|onlineBoardDeviceId" src/shell/DeviceContext.jsx | head -40
```
Expected: every term above appears in the value object exported by the Provider. If any term is missing, STOP and file a shell amendment — do NOT add it to the gallery file.

- [ ] **Step 3: Confirm `useToast` signature**

```bash
cd /path/to/claw-pet-manager/ref
grep -nE "useToast|push\(|dismiss" src/shell/ToastStack.jsx | head -20
```
Expected: `useToast` is exported, returns `{ push, dismiss, items }`, and the toast item shape includes `tone`, `title`, `message`, optional `action`, and optional `ttl`. This is what the new gallery will call.

- [ ] **Step 4: Build the assertion-migration ledger (mental, not written)**

For each existing test case in `AppearanceGallery.test.js`, decide:

| Existing case | Action |
|---|---|
| "gallery keeps the compact import actions" | REWRITE — assert the 3 creation labels live inside a `SplitButton` in `PageShell` actions, not in a `gallery-actions` div |
| "community import starts with the source website…" | KEEP — modal internals are unchanged |
| "setting a gallery item as desktop pet opens a channel selection step" | KEEP — modal flow is preserved; only update selectors that depend on local state to use context-driven names |
| "gallery badges list every channel configured for the same appearance" | KEEP — `configuredAgentIdsForAppearance` still drives the badge |
| "gallery set-as-desktop-pet action is gated by onboarding-style device connection checks" | REWRITE — checks now come from `useDeviceContext()`, not local `useState`/`useEffect`. Drop assertions on local `useState(false)` shape; add assertions on `useDeviceContext()` usage and `applyDesktopPet(` invocation |
| "gallery card previews fit width while Codex imports show the whole subject" | KEEP — CSS rules unchanged |
| "codex import rows use backend-generated preview images…" | KEEP — modal internals unchanged |
| "gallery metadata badges sit below media and active buttons keep the same shape" | REWRITE — selector becomes `appearance-card-tags` (unchanged) but `<CustomCard` becomes `<AppearanceCard`; assert renamed |
| "gallery cards pin desktop-pet actions to a uniform body height" | KEEP — CSS shape unchanged |
| "gallery uses cached appearance and Codex scans with explicit force refreshes" | REWRITE — `reload` still exists but the source of truth for `agentAppearanceMap` is the context; assertion about `saveAgentAppearanceMap` inside `reload` is dropped (context handles it via `applyDesktopPet`) |

This step exists so the implementer never silently deletes a behavioral guarantee. No code change here; only commit a placeholder note as part of Task 2’s first commit.

- [ ] **Step 5: No commit for this task** (planning only; nothing written to disk)

---

## Task 2: Replace local USB / online polling with `useDeviceContext()`

This task removes the duplicated polling loops without changing the rendered tree yet. It is the smallest reversible refactor that lets the rest of the plan stand on `useDeviceContext()` as the single source of truth.

**Files:**
- Modify: `ref/src/AppearanceGallery.jsx`
- Modify: `ref/src/AppearanceGallery.test.js`

- [ ] **Step 1: Write the failing test — context is the only source of USB/online state**

Open `ref/src/AppearanceGallery.test.js`. Replace the existing `"gallery set-as-desktop-pet action is gated by onboarding-style device connection checks"` test with this version:

```javascript
test("gallery reads USB/online/agent state from the shared device context (no local polling)", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  // The gallery no longer owns the USB / device-online polling loops.
  assert.doesNotMatch(gallery, /const \[usbConnected, setUsbConnected\] = useState/);
  assert.doesNotMatch(gallery, /const \[usbBoardDeviceId, setUsbBoardDeviceId\] = useState/);
  assert.doesNotMatch(gallery, /const \[deviceOnline, setDeviceOnline\] = useState/);
  assert.doesNotMatch(gallery, /const \[onlineBoardDeviceId, setOnlineBoardDeviceId\] = useState/);
  assert.doesNotMatch(gallery, /invoke\("usb_get_status"\)/);
  assert.doesNotMatch(gallery, /invoke\("usb_scan_devices"\)/);
  assert.doesNotMatch(gallery, /invoke\("check_device_availability"\)/);
  assert.doesNotMatch(gallery, /resolveOnlineBoardDeviceId/);

  // Everything reachable from the device dashboard is now pulled from context.
  assert.match(gallery, /useDeviceContext\(\)/);
  assert.match(
    gallery,
    /const\s*\{\s*[^}]*usb[^}]*deviceOnline[^}]*\}\s*=\s*useDeviceContext\(\)/,
  );

  // The shared apply helper replaces the inline applyDesktopPetAssignment call site.
  assert.match(gallery, /applyDesktopPet\(/);
  assert.doesNotMatch(gallery, /applyDesktopPetAssignment\(\{[\s\S]*invoke,[\s\S]*listen,/);
});
```

Also drop the now-obsolete asserts inside `"gallery uses cached appearance and Codex scans with explicit force refreshes"` that referenced `saveAgentAppearanceMap` and the local `loadAgentAppearanceMap` call site. Replace that whole test with:

```javascript
test("gallery uses cached appearance and Codex scans with explicit force refreshes", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  assert.match(gallery, /getCachedAppearances/);
  assert.match(gallery, /listAppearances\(\{ force \}\)/);
  assert.match(gallery, /reload\(\{ force: true \}\)/);
  assert.match(gallery, /from "\.\/lib\/codex-pets-client\.js"/);
  assert.match(gallery, /listCodexPets\(\)/);
  assert.match(gallery, /listCodexPets\(\{ force: true \}\)/);
  assert.doesNotMatch(gallery, /invoke\("list_codex_pets"\)/);
  assert.doesNotMatch(gallery, /invoke\("install_codex_community_pet"/);

  // The per-channel appearance map is no longer persisted inside reload — that's
  // the context's job; reload only refreshes the gallery view.
  assert.doesNotMatch(gallery, /saveAgentAppearanceMap\(nextMap\)/);
});
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: the two rewritten tests FAIL. The doesNotMatch assertions fire because the source still contains `const [usbConnected, setUsbConnected] = useState(false);` and friends.

- [ ] **Step 3: Refactor AppearanceGallery.jsx — wire the context, delete the polling loops**

Open `ref/src/AppearanceGallery.jsx`. Make these edits in order:

(a) **Replace the imports block** (lines ~12 to ~70). Replace the existing imports section with:

```jsx
import React, { useCallback, useEffect, useRef, useState } from "react";
import {
  RefreshCw,
  Loader,
  AlertCircle,
  Download,
  X,
  Globe,
  ExternalLink,
  CheckCircle,
  CheckCircle2,
  Sparkles,
  Terminal,
  UploadCloud,
  Code,
  Zap,
  ChevronDown,
  Check,
} from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import { getCachedAppearances, listAppearances } from "./lib/appearance-store.js";
import { listBuiltinAppearances } from "./lib/builtin-appearances.js";
import AppearancePreview from "./AppearancePreview.jsx";
import ChannelSwitchConfirmModal from "./ChannelSwitchConfirmModal.jsx";
import { resolveGalleryPreviewMedia } from "./lib/appearance-preview.js";
import {
  buildCodexPetSnapshot,
  findUpdatedCodexPets,
  formatCodexPetModifiedAt,
  parseCommunityPetImportInput,
  sortCodexPetsByModifiedAt,
} from "./lib/codex-community-import.js";
import {
  checkFfmpegAvailable,
  importCodexPet,
  installCodexCommunityPet,
  listCodexPets,
} from "./lib/codex-pets-client.js";
import {
  abortGenerationTask,
  subscribeGenerationTask,
} from "./lib/generation-task.js";
import {
  activeDesktopAssignment,
  channelLabelForId,
  pickFirstDetectedAgentId,
  shouldConfirmChannelSwitch,
} from "./lib/agent-appearance-config.js";
import {
  APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE,
  CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE,
} from "./lib/desktop-pet-assignment.js";
import PageShell from "./shell/PageShell.jsx";
import Card from "./shell/Card.jsx";
import { useDeviceContext } from "./shell/DeviceContext.jsx";
import { useToast } from "./shell/ToastStack.jsx";
```

Removed deliberately:
- `loadAgentAppearanceMap`, `loadEnabledAgents`, `saveAgentAppearanceMap`, `assignedAgentIds`, `normalizeDetectedAgents`, `FIXED_AGENT_OPTIONS` (context owns them now)
- `listen` from `@tauri-apps/api/event` (only `applyDesktopPet` from context calls into the listen-bearing helper now)
- `applyDesktopPetAssignment` from `./lib/desktop-pet-assignment.js` (used via context wrapper)
- `resolveOnlineBoardDeviceId` from `./lib/device-availability.js` (context owns it)

(b) **Replace the gallery state block** (lines ~109 to ~135). Replace the start of `export default function AppearanceGallery({ binding, onEnterWizard, onOpenDetail }) {` body with:

```jsx
export default function AppearanceGallery({ binding, onEnterWizard, onOpenDetail }) {
  const {
    usb,
    deviceOnline,
    deviceConnected,
    onlineBoardDeviceId,
    agentAppearanceMap,
    enabledAgents,
    agentOptions,
    currentDisplay,
    applyDesktopPet,
  } = useDeviceContext();
  const { push: pushToast } = useToast();

  const [items, setItems] = useState(() => getCachedAppearances() || null);
  const [error, setError] = useState("");
  const [refreshing, setRefreshing] = useState(false);

  // ── Codex import modal ──
  const [codexModalOpen, setCodexModalOpen] = useState(false);
  const [codexPets, setCodexPets] = useState(null);
  const [codexLoading, setCodexLoading] = useState(false);
  const [codexError, setCodexError] = useState("");
  const [importError, setImportError] = useState("");
  const [importingId, setImportingId] = useState("");
  const [syncingId, setSyncingId] = useState("");

  const [channelModal, setChannelModal] = useState(null);
  const [channelSwitchWarning, setChannelSwitchWarning] = useState(null);

  // ── Community import modal ──
  const [communityModalOpen, setCommunityModalOpen] = useState(false);
```

(c) **Simplify `reload`** (lines ~136 to ~159). Replace the existing `reload` `useCallback` with:

```jsx
  const reload = useCallback(async ({ force = false } = {}) => {
    setRefreshing(true);
    setError("");
    try {
      const records = await listAppearances({ force });
      setItems(records);
    } catch (err) {
      console.error(err);
      setError(err?.message || String(err));
      // Don't blank the grid on transient sync errors: keep whatever we already
      // had on screen, otherwise fall back to the built-in Westie/Terrier so
      // users never see an empty gallery.
      setItems((current) => (current && current.length > 0 ? current : listBuiltinAppearances()));
    } finally {
      setRefreshing(false);
    }
  }, []);
```

(d) **Delete the local `detect_local_agents` `useEffect`** (lines ~165 to ~180 in the original). Context handles agent detection.

(e) **Delete the USB polling `useEffect`** (lines ~252 to ~290 in the original).

(f) **Delete the device-online polling `useEffect`** (lines ~292 to ~328 in the original).

(g) **Replace `handleSetDesktopPet`** (lines ~362 to ~409 in the original) with a context-based version:

```jsx
  const handleSetDesktopPet = useCallback(async () => {
    if (!channelModal?.appearance?.id || !channelModal.agentId || syncingId) return;

    setSyncingId(channelModal.appearance.id);
    try {
      const { notice } = await applyDesktopPet(channelModal.agentId, channelModal.appearance, {
        onProgress: (progress) => {
          if (!progress) return;
          pushToast({
            tone: progress.type === "error" ? "error" : progress.type === "warning" ? "warning" : "info",
            title: progress.text,
            ttl: 4000,
          });
        },
      });
      pushToast({ tone: "success", title: notice || "形象已应用", ttl: 5000 });
      setChannelModal(null);
    } catch (err) {
      console.error(err);
      const message = err?.message || String(err);
      const tone =
        message === APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE ||
        message === CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE
          ? "warning"
          : "error";
      pushToast({ tone, title: message, ttl: 6000 });
    } finally {
      setSyncingId("");
    }
  }, [applyDesktopPet, channelModal, pushToast, syncingId]);
```

(h) **Adjust the channel-modal gating logic** (lines ~330 to ~344 in the original). The derived booleans now lean on context state. Replace with:

```jsx
  const activeAppearanceId = currentDisplay.appearance?.id || "";
  const channelModalRequiresUsb = Boolean(
    channelModal?.appearance?.id && channelModal.appearance.id !== activeAppearanceId,
  );
  const channelModalCanApply = channelModalRequiresUsb ? usb.connected : deviceConnected;
  const channelModalConnectionMessage = channelModalRequiresUsb
    ? APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE
    : CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE;

  const openChannelModal = useCallback((row) => {
    const activeAgentId = activeDesktopAssignment(agentAppearanceMap, enabledAgents).agentId;
    const detectedAgentId = pickFirstDetectedAgentId(agentOptions, activeAgentId);
    setChannelModal({ appearance: row, agentId: activeAgentId || detectedAgentId || "codex" });
  }, [agentAppearanceMap, agentOptions, enabledAgents]);

  const requestChannelChange = useCallback((agentId) => {
    if (shouldConfirmChannelSwitch(agentAppearanceMap, agentId, enabledAgents)) {
      setChannelSwitchWarning({ agentId });
      return;
    }
    setChannelModal((prev) => prev ? { ...prev, agentId } : prev);
  }, [agentAppearanceMap, enabledAgents]);

  const confirmChannelSwitch = useCallback(() => {
    const agentId = channelSwitchWarning?.agentId;
    if (agentId) {
      setChannelModal((prev) => prev ? { ...prev, agentId } : prev);
    }
    setChannelSwitchWarning(null);
  }, [channelSwitchWarning]);
```

(i) **Delete the `syncNotice` local state and its inline-style floating banner**. In particular: remove the line `const [syncNotice, setSyncNotice] = useState(null);` and remove the entire JSX block beginning `{syncNotice && (` and ending with its closing `)}` (lines ~503 to ~525 in the original). Toasts replace it.

At this point the existing JSX still mentions `CustomCard`, `usbConnected`, etc. — fix the call site:

(j) **Update the `CustomCard` call site** (lines ~487 to ~500 in the original). Replace with:

```jsx
        <div className="appearance-grid">
          {items.map((row) => (
            <CustomCard
              key={row.id}
              row={row}
              isActive={row.id === activeAppearanceId}
              disabled={false}
              configuredAgentIds={configuredAgentIdsForAppearance(agentAppearanceMap, row.id)}
              agentOptions={agentOptions}
              isSyncing={syncingId === row.id}
              usbConnected={usb.connected}
              onOpenDetail={onOpenDetail}
              onSelectChannel={openChannelModal}
            />
          ))}
        </div>
```

(`CustomCard` is renamed to `AppearanceCard` in Task 4 — leave that for the next task to keep this commit small.)

(k) **Replace the `onlineBoardDeviceId` / `usbBoardDeviceId` references that remain elsewhere**. After deleting the local state, the only references should be inside the (now-deleted) old `handleSetDesktopPet`. Verify with:

```bash
cd /path/to/claw-pet-manager/ref
grep -nE "usbConnected|usbBoardDeviceId|onlineBoardDeviceId|setUsbConnected|setDeviceOnline" src/AppearanceGallery.jsx
```
Expected: only the `usb.connected` access inside the `<CustomCard>` call site (passed through as the `usbConnected` prop name for now — that prop stays inside `CustomCard` for its hover tooltip). No `setUsb*` or `setDeviceOnline` setters. No `useState(false)` for any of them.

`onlineBoardDeviceId` is destructured from context but not used directly in this file (it’s captured inside `applyDesktopPet`). That’s fine — keep it destructured so the upcoming `AppearanceCard` rename / future read paths don’t need to touch the destructure line.

Actually — to avoid an unused-binding warning, drop `onlineBoardDeviceId` from the destructure:

```jsx
  const {
    usb,
    deviceOnline,
    deviceConnected,
    agentAppearanceMap,
    enabledAgents,
    agentOptions,
    currentDisplay,
    applyDesktopPet,
  } = useDeviceContext();
```

`deviceOnline` is used by `channelModalCanApply` via `deviceConnected`; keep it destructured for symmetry with the dashboard or remove it. Drop it:

```jsx
  const {
    usb,
    deviceConnected,
    agentAppearanceMap,
    enabledAgents,
    agentOptions,
    currentDisplay,
    applyDesktopPet,
  } = useDeviceContext();
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: the two rewritten tests now PASS. Other existing tests that still expect old structure (e.g. the gallery-actions div, CustomCard naming, sync-notice) will still pass for now since those code paths haven’t been touched yet — they get rewritten in Tasks 3 and 4.

Note: the test "gallery set-as-desktop-pet action is gated by onboarding-style device connection checks" was REPLACED in Step 1 with the new test "gallery reads USB/online/agent state from the shared device context (no local polling)". The other tests should still report PASS.

- [ ] **Step 5: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/AppearanceGallery.jsx ref/src/AppearanceGallery.test.js
git commit -m "feat(gallery): 切换至 DeviceContext，删除画廊本地 USB/在线轮询"
```

---

## Task 3: Replace `sync-notice` floating banner with `useToast()`

Task 2 already pushed toasts inside `handleSetDesktopPet`. This task removes the residual inline-style fallback and adds the missing test.

**Files:**
- Modify: `ref/src/AppearanceGallery.jsx`
- Modify: `ref/src/AppearanceGallery.test.js`

- [ ] **Step 1: Write the failing test**

Append to `ref/src/AppearanceGallery.test.js`:

```javascript
test("gallery surfaces sync results through the shared toast queue", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  // The old inline-style floating banner is gone.
  assert.doesNotMatch(gallery, /sync-notice/);
  assert.doesNotMatch(gallery, /setSyncNotice/);
  assert.doesNotMatch(gallery, /position:\s*"fixed"[\s\S]*bottom:\s*24/);

  // The shared toast hook from Plan 1 replaces it.
  assert.match(gallery, /useToast\(\)/);
  assert.match(gallery, /pushToast\(\{\s*tone:\s*"success"/);
});
```

- [ ] **Step 2: Run test to verify it fails (if any residual `sync-notice` remained)**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: if Task 2 already removed `setSyncNotice` and the inline banner JSX, this test should PASS immediately. If it fails (e.g. on the `pushToast({ tone: "success"` regex because the implementation used a different quote style), fix the implementation — the regex is the source of truth.

If the test PASSES on the first run, that’s an indicator Task 2 was clean and this commit is documentation-only. Proceed.

- [ ] **Step 3: If the test failed, fix the residual code**

Search and remove any leftover:
```bash
cd /path/to/claw-pet-manager/ref
grep -nE "syncNotice|sync-notice|setSyncNotice" src/AppearanceGallery.jsx
```
Expected: empty output. Any remaining hit indicates leftover code from Task 2 — delete it.

- [ ] **Step 4: Run tests to verify they pass**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: ALL PASS.

- [ ] **Step 5: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/AppearanceGallery.jsx ref/src/AppearanceGallery.test.js
git commit -m "feat(gallery): 同步反馈走 useToast，删除内联浮层 sync-notice"
```

---

## Task 4: Rename `CustomCard` → `AppearanceCard`, add `isActive` + “使用中” badge

The card naming reflects the simplified responsibilities (browse + one-tap apply). The `isActive` prop drives both a chip badge and an `is-active` border on the card.

**Files:**
- Modify: `ref/src/AppearanceGallery.jsx`
- Modify: `ref/src/AppearanceGallery.test.js`
- Modify: `ref/src/styles.css`

- [ ] **Step 1: Write the failing test**

In `ref/src/AppearanceGallery.test.js`, replace the existing test `"gallery metadata badges sit below media and active buttons keep the same shape"` with:

```javascript
test("gallery cards surface the active appearance with a 使用中 badge and is-active border", () => {
  const gallery = readSource("AppearanceGallery.jsx");
  const css = readSource("styles.css");

  // The card is renamed and reflects browse + one-tap apply responsibilities.
  assert.doesNotMatch(gallery, /function CustomCard\(/);
  assert.match(gallery, /function AppearanceCard\(/);
  assert.match(gallery, /<AppearanceCard\b/);

  // Active state is driven from context-derived currentDisplay.
  assert.match(gallery, /isActive=\{row\.id === activeAppearanceId\}/);
  assert.match(gallery, /const activeAppearanceId = currentDisplay\.appearance\?\.id \|\| "";/);

  // Active appearance is visible inside the card body.
  assert.match(gallery, /使用中/);
  assert.match(gallery, /appearance-card--active|appearance-card\.is-active/);
  assert.match(gallery, /appearance-card__badge--active/);

  // Existing metadata layout assertions still hold.
  assert.match(gallery, /<AppearancePreview[\s\S]*<\/div>\s*<div className="appearance-card-body">[\s\S]*className="appearance-card-tags"/);

  // CSS encodes the active border treatment.
  const activeCard = extractCssRule(css, ".appearance-card.is-active");
  assert.match(activeCard, /border-color:\s*var\(--accent\);/);
});
```

Also rename the inner-callback `onSelectChannel` to `onSetAsDesktopPet` in the asserts. Add a fresh test for the rename:

```javascript
test("AppearanceCard exposes onSetAsDesktopPet (not onSelectChannel) for the apply-to-desktop CTA", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  assert.match(gallery, /onSetAsDesktopPet=\{openChannelModal\}/);
  assert.doesNotMatch(gallery, /onSelectChannel=\{openChannelModal\}/);
  assert.match(gallery, /onSetAsDesktopPet\?\.\(row\)/);
});
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: the two new tests FAIL because `CustomCard` is still the name and there is no `isActive` / `使用中` markup yet.

- [ ] **Step 3: Rename `CustomCard` → `AppearanceCard` and add the active badge**

In `ref/src/AppearanceGallery.jsx`:

(a) Replace the call site:

```jsx
        <div className="appearance-grid">
          {items.map((row) => (
            <AppearanceCard
              key={row.id}
              row={row}
              isActive={row.id === activeAppearanceId}
              disabled={false}
              configuredAgentIds={configuredAgentIdsForAppearance(agentAppearanceMap, row.id)}
              agentOptions={agentOptions}
              isSyncing={syncingId === row.id}
              usbConnected={usb.connected}
              onOpenDetail={onOpenDetail}
              onSetAsDesktopPet={openChannelModal}
            />
          ))}
        </div>
```

(b) Replace the bottom-of-file `function CustomCard(...)` definition with:

```jsx
function AppearanceCard({
  row,
  isActive,
  disabled,
  configuredAgentIds,
  agentOptions,
  isSyncing,
  usbConnected,
  onOpenDetail,
  onSetAsDesktopPet,
}) {
  const [previewActive, setPreviewActive] = useState(false);
  const okCount = row.families?.filter?.((f) => f.ok).length || 0;
  const totalCount = row.families?.length || 0;
  const isCodex = row.type === "codex-import";
  const isBuiltin = row.type === "builtin";
  const previewMedia = resolveGalleryPreviewMedia(row);
  const isConfigured = configuredAgentIds.length > 0;
  const configuredLabel = configuredAgentIds.map((agentId) => channelLabelForId(agentOptions, agentId)).join("、");

  return (
    <article
      className={`appearance-card appearance-card--clickable${isActive ? " is-active" : ""}`}
      onPointerEnter={() => setPreviewActive(true)}
      onPointerLeave={() => setPreviewActive(false)}
    >
      <div
        className={`appearance-channel-preview appearance-card-preview${isCodex ? " appearance-card-preview--codex" : ""}`}
        role="button"
        tabIndex={0}
        onClick={() => onOpenDetail?.(row.id)}
        onKeyDown={(e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); onOpenDetail?.(row.id); } }}
      >
        <AppearancePreview
          media={previewMedia}
          className="appearance-channel-preview__media"
          emptyClassName="appearance-channel-preview__empty"
          playing={previewActive}
        />
        {isActive && (
          <span className="appearance-card__badge appearance-card__badge--active">
            <CheckCircle2 size={12} /> 使用中
          </span>
        )}
      </div>
      <div className="appearance-card-body">
        <div
          className="appearance-card-main"
          role="button"
          tabIndex={0}
          onClick={() => onOpenDetail?.(row.id)}
          onKeyDown={(e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); onOpenDetail?.(row.id); } }}
        >
          <h4>{row.name}</h4>
          <p>{row.description || `${row.provider} · ${row.model || "—"}`}</p>
          <div className="muted small">
            {okCount}/{totalCount} 个动画可用 · {new Date(row.created_at).toLocaleString()}
          </div>
          <div className="appearance-card-tags">
            <span className="appearance-thumb__badge">
              {isBuiltin ? "内置" : isCodex ? "Codex" : "自定义"}
            </span>
            {isConfigured && (
              <span className="appearance-card-assigned">
                <CheckCircle2 size={13} /> {configuredLabel}
              </span>
            )}
          </div>
        </div>
        <button
          type="button"
          className={`desktop-pet-btn${isConfigured ? " is-active" : ""}`}
          disabled={disabled || isSyncing || okCount === 0}
          onClick={(event) => {
            event.stopPropagation();
            onSetAsDesktopPet?.(row);
          }}
          title={disabled ? "当前形象没有可用动画" : isConfigured ? `已配置给 ${configuredLabel}` : usbConnected ? "打开设置形象弹窗，确认后通过 USB 同步到设备" : "打开设置形象弹窗，确认后同步到设备"}
        >
          {isSyncing ? (
            <>
              <Loader size={14} className="spin" /> {usbConnected ? "USB 传输中…" : "同步中…"}
            </>
          ) : isConfigured ? (
            <>
              <CheckCircle2 size={14} /> 设为桌宠
            </>
          ) : (
            <>
              <UploadCloud size={14} /> 设为桌宠
            </>
          )}
        </button>
      </div>
    </article>
  );
}
```

Note: the CTA label is unified to "设为桌宠" (spec § "Plan 3" example) for both first-configure and reconfigure cases — the active state in `is-active` styling and the configured-label tag below the title carries the “already configured for X” signal without the button text needing to change.

- [ ] **Step 4: Add CSS for `is-active` border and active badge**

In `ref/src/styles.css`, find the existing `.appearance-card` rule block (search `.appearance-card {`). Immediately AFTER its closing brace, append:

```css
.appearance-card.is-active {
  border-color: var(--accent);
  box-shadow: 0 0 0 1px var(--accent-soft, rgba(249, 115, 22, 0.18));
}

.appearance-card__badge {
  position: absolute;
  top: 8px;
  left: 8px;
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 2px 8px;
  border-radius: 999px;
  font-size: 11px;
  font-weight: 600;
  z-index: 2;
}

.appearance-card__badge--active {
  background: var(--accent);
  color: var(--text-on-accent, #fff);
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.15);
}
```

The `.appearance-card-preview` already has `position: relative` (existing rule) so the absolute-positioned badge anchors inside the preview frame. If a quick grep shows it does NOT have `position: relative`, add it:

```bash
cd /path/to/claw-pet-manager/ref
grep -n "\.appearance-card-preview\s*{" src/styles.css
```
Then if needed extend that rule with `position: relative;`.

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: the rename + active-badge tests now PASS. Existing tests (gallery-actions, channel selection, etc.) still PASS — they reference `<AppearanceCard` via the renamed assertions but the structural CSS / nested JSX guarantees are preserved.

If the test "setting a gallery item as desktop pet opens a channel selection step" fails because it greps for `onSetDesktopPet?.(row.id)` (an old negative assertion), update its `assert.doesNotMatch` line from `/onSetDesktopPet\?\.\(row\.id\)/` to `/onSelectChannel=\{openChannelModal\}/` since `onSelectChannel` is the new forbidden name and `onSetAsDesktopPet` is the new one.

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/AppearanceGallery.jsx ref/src/AppearanceGallery.test.js ref/src/styles.css
git commit -m "feat(gallery): AppearanceCard 重命名 + 使用中 徽章与 isActive 边框"
```

---

## Task 5: Build the local `SplitButton` helper

`SplitButton` is a primary-action label + chevron toggle that opens a dropdown of secondary actions. The 3 creation entrypoints collapse into a single SplitButton: clicking the main label triggers the first action (新建自定义形象 — the most common), clicking the chevron opens a menu with all 3.

**Files:**
- Modify: `ref/src/AppearanceGallery.jsx`
- Modify: `ref/src/AppearanceGallery.test.js`
- Modify: `ref/src/styles.css`

- [ ] **Step 1: Write the failing test**

Append to `ref/src/AppearanceGallery.test.js`:

```javascript
test("gallery exposes the 3 creation flows through a local SplitButton helper", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  // Local helper (NOT imported from shell — shell is frozen by Plan 1).
  assert.match(gallery, /function SplitButton\(/);
  assert.doesNotMatch(gallery, /from "\.\/shell\/SplitButton/);

  // Three creation labels live inside the SplitButton items array.
  assert.match(gallery, /新建自定义形象/);
  assert.match(gallery, /从 Codex 导入/);
  assert.match(gallery, /从社区导入/);

  // The chevron toggles a menu using local useState — no global popover dep.
  assert.match(gallery, /ChevronDown/);
  assert.match(gallery, /split-button__menu/);
  assert.match(gallery, /split-button__item/);
});

test("SplitButton menu closes on outside click and key escape", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  // Outside click via document mousedown listener.
  assert.match(gallery, /addEventListener\("mousedown"/);
  // Escape key closes via a keydown listener.
  assert.match(gallery, /key === "Escape"/);
});
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: the two new tests FAIL — no `function SplitButton(` and no `split-button__menu` class exist yet.

- [ ] **Step 3: Add the `SplitButton` helper at the bottom of `AppearanceGallery.jsx`**

Add this function ABOVE the existing `function AppearanceChannelModal(` definition (so all top-level helpers sit together at the bottom of the file):

```jsx
function SplitButton({ label, primaryAction, items, disabled }) {
  const [open, setOpen] = useState(false);
  const containerRef = useRef(null);

  useEffect(() => {
    if (!open) return undefined;
    const handleMouseDown = (event) => {
      if (containerRef.current && !containerRef.current.contains(event.target)) {
        setOpen(false);
      }
    };
    const handleKeyDown = (event) => {
      if (event.key === "Escape") setOpen(false);
    };
    document.addEventListener("mousedown", handleMouseDown);
    document.addEventListener("keydown", handleKeyDown);
    return () => {
      document.removeEventListener("mousedown", handleMouseDown);
      document.removeEventListener("keydown", handleKeyDown);
    };
  }, [open]);

  return (
    <div ref={containerRef} className={`split-button${open ? " is-open" : ""}`}>
      <button
        type="button"
        className="btn-primary btn-sm split-button__primary"
        onClick={primaryAction}
        disabled={disabled}
      >
        <Sparkles size={14} />
        <span>{label}</span>
      </button>
      <button
        type="button"
        className="btn-primary btn-sm split-button__chevron"
        onClick={() => setOpen((prev) => !prev)}
        disabled={disabled}
        aria-haspopup="menu"
        aria-expanded={open}
        aria-label="更多创建方式"
      >
        <ChevronDown size={14} />
      </button>
      {open && (
        <div className="split-button__menu" role="menu">
          {items.map((item) => (
            <button
              key={item.id}
              type="button"
              role="menuitem"
              className="split-button__item"
              onClick={() => {
                setOpen(false);
                item.onClick?.();
              }}
            >
              {item.icon}
              <span>{item.label}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
```

- [ ] **Step 4: Add CSS for the SplitButton**

In `ref/src/styles.css`, append to the bottom (after the existing `.gallery-action` rules — leaving the old `.gallery-actions` block in place per the spec’s out-of-scope GC deferral):

```css
.split-button {
  position: relative;
  display: inline-flex;
  align-items: stretch;
  gap: 0;
}

.split-button__primary {
  border-top-right-radius: 0;
  border-bottom-right-radius: 0;
  padding-right: 10px;
}

.split-button__chevron {
  border-top-left-radius: 0;
  border-bottom-left-radius: 0;
  border-left: 1px solid rgba(255, 255, 255, 0.25);
  padding-left: 8px;
  padding-right: 8px;
}

.split-button__menu {
  position: absolute;
  top: calc(100% + 6px);
  right: 0;
  min-width: 200px;
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: var(--radius-md);
  box-shadow: 0 6px 18px rgba(0, 0, 0, 0.12);
  padding: 4px;
  z-index: 30;
  display: flex;
  flex-direction: column;
}

.split-button__item {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 8px 10px;
  border: none;
  background: transparent;
  border-radius: var(--radius-sm);
  color: var(--text);
  font-size: 13px;
  cursor: pointer;
  text-align: left;
}

.split-button__item:hover {
  background: var(--surface-muted);
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: the two new SplitButton tests PASS. All other tests remain passing.

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/AppearanceGallery.jsx ref/src/AppearanceGallery.test.js ref/src/styles.css
git commit -m "feat(gallery): 新增 SplitButton 局部组件承载三种创建方式"
```

---

## Task 6: Replace the page chrome with `<PageShell>`

Now the building blocks are in place — context, toasts, AppearanceCard, SplitButton — the page chrome flips from the old three-row `page-toolbar` + `page-hero` + `gallery-actions` stack to a single `<PageShell>` with two actions.

**Files:**
- Modify: `ref/src/AppearanceGallery.jsx`
- Modify: `ref/src/AppearanceGallery.test.js`

- [ ] **Step 1: Write the failing test**

In `ref/src/AppearanceGallery.test.js`, replace the existing `"gallery keeps the compact import actions"` test with this version:

```javascript
test("gallery renders inside <PageShell> with refresh + SplitButton actions", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  // Shell-driven chrome — no more triple stack of page-toolbar / page-hero / gallery-actions.
  assert.match(gallery, /import PageShell from "\.\/shell\/PageShell\.jsx"/);
  assert.match(gallery, /<PageShell\b[\s\S]*title="形象画廊"/);
  assert.doesNotMatch(gallery, /<div className="page-toolbar"/);
  assert.doesNotMatch(gallery, /<div className="page-hero"/);
  assert.doesNotMatch(gallery, /<div className="gallery-actions"/);

  // Refresh stays as an independent PageShell action (spec § "刷新按钮独立").
  assert.match(gallery, /actions=\{\[/);
  assert.match(gallery, /onClick=\{\(\) => reload\(\{ force: true \}\)\}[\s\S]*RefreshCw/);

  // The 3 creation flows are inside SplitButton, not parallel siblings.
  assert.match(gallery, /<SplitButton[\s\S]*label="添加形象"/);
});
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: the new test FAILS — no `<PageShell` usage yet, and `<div className="page-toolbar"` / `<div className="page-hero"` / `<div className="gallery-actions"` still exist.

- [ ] **Step 3: Rewrite the gallery render tree to use `<PageShell>`**

In `ref/src/AppearanceGallery.jsx`, replace the entire `return (...)` of the default `AppearanceGallery` function with:

```jsx
  const refreshButton = (
    <button
      key="refresh"
      type="button"
      className="btn-secondary btn-sm"
      onClick={() => reload({ force: true })}
      disabled={refreshing}
    >
      <RefreshCw size={14} className={refreshing ? "spin" : undefined} />
      刷新
    </button>
  );

  const addAppearanceSplitButton = (
    <SplitButton
      key="add"
      label="添加形象"
      primaryAction={onEnterWizard}
      items={[
        {
          id: "wizard",
          label: "新建自定义形象",
          icon: <Sparkles size={14} />,
          onClick: onEnterWizard,
        },
        {
          id: "codex",
          label: "从 Codex 导入",
          icon: <Download size={14} />,
          onClick: openCodexImport,
        },
        {
          id: "community",
          label: "从社区导入",
          icon: <Globe size={14} />,
          onClick: () => setCommunityModalOpen(true),
        },
      ]}
    />
  );

  return (
    <PageShell
      title="形象画廊"
      subtitle="浏览默认形象与你的自定义形象，进入详情可预览每个 family 的视频。"
      actions={[refreshButton, addAppearanceSplitButton]}
    >
      {error && (
        <div className="message-banner message-banner--error">
          <AlertCircle size={14} /> 读取形象失败：{error}
        </div>
      )}

      {taskRunning && (
        <Card>
          <RunningTaskCard
            task={task}
            onAbort={abortGenerationTask}
            onOpenDetail={onOpenDetail}
          />
        </Card>
      )}

      {items === null ? (
        <div className="empty-state">
          <Loader size={20} className="spin" />
          <div>
            <strong>正在加载形象列表…</strong>
          </div>
        </div>
      ) : items.length === 0 ? (
        <div className="empty-state">
          <div>
            <strong>还没有自定义形象</strong>
          </div>
          <div className="muted small">
            点击上方「添加形象」上传一张图，生成属于你的 10 段桌宠动画。
          </div>
        </div>
      ) : (
        <div className="appearance-grid">
          {items.map((row) => (
            <AppearanceCard
              key={row.id}
              row={row}
              isActive={row.id === activeAppearanceId}
              disabled={false}
              configuredAgentIds={configuredAgentIdsForAppearance(agentAppearanceMap, row.id)}
              agentOptions={agentOptions}
              isSyncing={syncingId === row.id}
              usbConnected={usb.connected}
              onOpenDetail={onOpenDetail}
              onSetAsDesktopPet={openChannelModal}
            />
          ))}
        </div>
      )}

      {codexModalOpen && (
        <CodexImportModal
          loading={codexLoading}
          pets={codexPets}
          error={codexError || importError}
          importingId={importingId}
          onClose={() => {
            if (importingId) return;
            setCodexModalOpen(false);
          }}
          onPick={(petId) => handleImportPet(petId, { closeModal: "codex" })}
        />
      )}

      {communityModalOpen && (
        <CommunityImportModal
          importingId={importingId}
          importError={importError}
          onClose={() => {
            if (importingId) return;
            setCommunityModalOpen(false);
          }}
          onImport={(petId) => handleImportPet(petId, { closeModal: "community" })}
        />
      )}

      {channelModal && (
        <AppearanceChannelModal
          appearance={channelModal.appearance}
          agentId={channelModal.agentId}
          agentOptions={agentOptions}
          configuredAgentIds={configuredAgentIdsForAppearance(agentAppearanceMap, channelModal.appearance.id)}
          disabled={!channelModalCanApply}
          deviceConnected={channelModalCanApply}
          deviceConnectionMessage={channelModalConnectionMessage}
          syncing={syncingId === channelModal.appearance.id}
          onChange={requestChannelChange}
          onClose={() => {
            if (syncingId) return;
            setChannelModal(null);
            setChannelSwitchWarning(null);
          }}
          onConfirm={handleSetDesktopPet}
        />
      )}

      {channelSwitchWarning && (
        <ChannelSwitchConfirmModal
          currentLabel={channelLabelForId(agentOptions, activeDesktopAssignment(agentAppearanceMap, enabledAgents).agentId)}
          nextLabel={channelLabelForId(agentOptions, channelSwitchWarning.agentId)}
          onCancel={() => setChannelSwitchWarning(null)}
          onConfirm={confirmChannelSwitch}
        />
      )}
    </PageShell>
  );
}
```

Note the outer `<div className="page page-appearance-gallery">` and the inline-styled `page-toolbar` / `page-hero` / `gallery-actions` blocks are all gone — `PageShell` owns the page wrapper now (its outer div is `class="page page-shell"`, per Plan 1 Task 2).

- [ ] **Step 4: Run tests to verify they pass**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: ALL PASS. If the "community import" or "channel-selection step" tests now fail because they grep for an outer `<div className="page page-appearance-gallery">` wrapper (they should not — re-read them), update those grep targets to the corresponding child elements that still exist (`.appearance-grid`, `function AppearanceChannelModal`, `function CommunityImportModal`).

- [ ] **Step 5: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/AppearanceGallery.jsx ref/src/AppearanceGallery.test.js
git commit -m "feat(gallery): 切换至 PageShell，刷新与添加形象进入 actions 槽"
```

---

## Task 7: Wrap `RunningTaskCard` in shell `<Card>` (visual unification)

`RunningTaskCard` already renders a self-styled card. To match the spec — every section is a `<Card>` — wrap it once at the call site. The internal implementation of `RunningTaskCard` is unchanged.

This is already done as part of Task 6 (`<Card><RunningTaskCard .../></Card>`). This task adds a guard test that locks the behavior so future refactors don’t accidentally drop the wrapper.

**Files:**
- Modify: `ref/src/AppearanceGallery.test.js`

- [ ] **Step 1: Write the test**

Append to `ref/src/AppearanceGallery.test.js`:

```javascript
test("running task card is wrapped in a shell <Card> and only rendered while a task is in flight", () => {
  const gallery = readSource("AppearanceGallery.jsx");

  assert.match(gallery, /import Card from "\.\/shell\/Card\.jsx"/);
  assert.match(
    gallery,
    /\{taskRunning && \(\s*<Card>\s*<RunningTaskCard/,
  );
  assert.match(gallery, /const taskRunning = task\?\.status === "running";/);
});
```

- [ ] **Step 2: Run tests**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: the new test PASSES (the wrapper was added in Task 6). If it fails on the regex (whitespace / formatting differences), tighten the regex — do NOT add/remove the wrapper.

- [ ] **Step 3: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/AppearanceGallery.test.js
git commit -m "test(gallery): RunningTaskCard 包裹于 <Card> 的回归断言"
```

---

## Task 8: Visual rework of `AppearanceChannelModal` (preserve logic)

Per spec § "Plan 3 / 关键决定": `AppearanceChannelModal` is preserved and only visually reworked to match the new design tokens. The logic — selecting a channel, gating apply by USB/online state, surfacing the channel-switch warning — is unchanged.

The visual rework is a minimal touch: the existing `.appearance-channel-modal__body` / `__list` / `__notice` / `__actions` selectors already use design tokens (`var(--line)`, `var(--accent)`, `var(--radius-*)`). The only required change is to make the modal opt into the shell `Card` look-and-feel by adding a `card`-equivalent surface class on the outer card and letting the existing classes provide the inner padding/layout.

**Files:**
- Modify: `ref/src/AppearanceGallery.jsx`
- Modify: `ref/src/AppearanceGallery.test.js`
- Modify: `ref/src/styles.css`

- [ ] **Step 1: Write the failing test**

Append to `ref/src/AppearanceGallery.test.js`:

```javascript
test("AppearanceChannelModal preserves its logic and adopts shell card chrome", () => {
  const gallery = readSource("AppearanceGallery.jsx");
  const css = readSource("styles.css");

  // Logic preserved — same props, same internal flow.
  assert.match(gallery, /function AppearanceChannelModal\(/);
  assert.match(gallery, /选择要使用这个形象的渠道/);
  assert.match(gallery, /role="radiogroup"/);
  assert.match(gallery, /onConfirm=\{handleSetDesktopPet\}/);

  // Visual: outer card uses the modal-card--channel class with refreshed surface tokens.
  assert.match(gallery, /modal-card modal-card--channel/);

  // Surface tokens match the shell Card look-and-feel.
  const channelModal = extractCssRule(css, ".modal-card--channel");
  assert.match(channelModal, /border-radius:\s*var\(--radius-lg\);/);
  assert.match(channelModal, /background:\s*var\(--surface\);/);
});
```

- [ ] **Step 2: Run test to verify it fails (or passes if CSS already matches)**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: the test FAILS if `.modal-card--channel` does not yet use `--radius-lg` / `--surface`. Check the existing rule:

```bash
cd /path/to/claw-pet-manager/ref
grep -nA 8 "\.modal-card--channel" src/styles.css
```

- [ ] **Step 3: Update the `.modal-card--channel` rule**

If the existing rule does not assert the tokens above, modify the existing block (do NOT add a new sibling — keep the rule single-sourced). Example replacement:

```css
.modal-card--channel {
  background: var(--surface);
  border-radius: var(--radius-lg);
  box-shadow: var(--shadow-md, 0 12px 32px rgba(0, 0, 0, 0.18));
  border: 1px solid var(--line);
  max-width: 480px;
}
```

If `.modal-card--channel` does not exist at all, add the block above to the shell section or near the existing `.modal-card` rule.

The JSX of `AppearanceChannelModal` itself is unchanged from the existing implementation — keep it byte-for-byte from the original except that its className already includes `modal-card--channel`. Verify:

```bash
cd /path/to/claw-pet-manager/ref
grep -n "modal-card--channel" src/AppearanceGallery.jsx
```
Expected: exactly one hit, inside `AppearanceChannelModal`.

- [ ] **Step 4: Run tests to verify they pass**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: ALL PASS.

- [ ] **Step 5: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/AppearanceGallery.jsx ref/src/styles.css ref/src/AppearanceGallery.test.js
git commit -m "feat(gallery): AppearanceChannelModal 视觉对齐 shell Card 令牌"
```

---

## Task 9: Smoke test the full page in dev and verify shell tests still pass

This catches any cross-cutting regression: a missing import, a stale `binding` prop wiring, a CSS token that does not resolve, etc.

**Files:** none modified (verification only).

- [ ] **Step 1: Run the full gallery test file**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/AppearanceGallery.test.js
```
Expected: ALL PASS. The full list of expected passing test names:
- gallery renders inside <PageShell> with refresh + SplitButton actions
- community import starts with the source website before showing two methods
- setting a gallery item as desktop pet opens a channel selection step
- gallery badges list every channel configured for the same appearance
- gallery reads USB/online/agent state from the shared device context (no local polling)
- gallery card previews fit width while Codex imports show the whole subject
- codex import rows use backend-generated preview images instead of local file backgrounds
- gallery cards surface the active appearance with a 使用中 badge and is-active border
- AppearanceCard exposes onSetAsDesktopPet (not onSelectChannel) for the apply-to-desktop CTA
- gallery cards pin desktop-pet actions to a uniform body height
- gallery uses cached appearance and Codex scans with explicit force refreshes
- gallery surfaces sync results through the shared toast queue
- gallery exposes the 3 creation flows through a local SplitButton helper
- SplitButton menu closes on outside click and key escape
- running task card is wrapped in a shell <Card> and only rendered while a task is in flight
- AppearanceChannelModal preserves its logic and adopts shell card chrome

- [ ] **Step 2: Run shell tests to confirm we did not modify any shell file**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/*.test.js
```
Expected: ALL PASS (no shell file changed by Plan 3). If any fail, you accidentally touched a shell file — `git diff src/shell/` should be empty; revert any change there.

```bash
cd /path/to/claw-pet-manager
git diff --stat ref/src/shell/
```
Expected: empty output (no shell files staged or modified).

- [ ] **Step 3: Run sibling page tests for regression check**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/DeviceDashboard.test.js src/ComponentCenter.test.js src/AppearanceDetail.test.js src/ProductExperience.test.js src/DeviceGuideModal.test.js src/DeviceSetup.test.js src/CustomAvatarWizard.test.js
```
Expected: ALL PASS. Plan 3 does not touch those pages — failures here mean accidental cross-contamination.

- [ ] **Step 4: Run all lib tests**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/lib/*.test.js
```
Expected: ALL PASS. Plan 3 only consumes lib helpers, never modifies them.

- [ ] **Step 5: Smoke test in dev**

```bash
cd /path/to/claw-pet-manager/ref
npm run dev:web
```
Open `http://localhost:4173`. Navigate to the gallery (sidebar → 形象画廊). Verify:
- Page header reads "形象画廊" with subtitle below
- Top-right shows: a [刷新] button, then a [添加形象 ▾] split button
- Clicking the main [添加形象] label opens the custom avatar wizard
- Clicking the chevron opens a menu with 3 items (新建自定义形象 / 从 Codex 导入 / 从社区导入)
- Clicking each menu item triggers the correct flow (wizard / Codex modal / community modal)
- Each card with `okCount > 0` shows the right border + “使用中” chip when it’s the currently displayed appearance (verify by binding a device and applying)
- Clicking 设为桌宠 on a card opens `AppearanceChannelModal`; selecting a channel and confirming triggers an apply and surfaces a toast at the bottom of the screen (NOT the old inline floating banner)
- If `currentDisplay.appearance` changes (e.g. another agent picks a different appearance via dashboard in another window), the "使用中" badge moves to the new card on next render
- No console errors related to missing context fields or undefined `usb` / `currentDisplay`

Ctrl+C the dev server when done.

- [ ] **Step 6: No commit** (verification only)

---

## Task 10: Update folder map

**Files:**
- Modify: `ref/src/.folder.md`

- [ ] **Step 1: Update the `AppearanceGallery.jsx` row**

Open `ref/src/.folder.md`. Find the row starting with `` | `AppearanceGallery.jsx` ``. Replace its Function blurb with:

```markdown
| `AppearanceGallery.jsx` | `component` | Pet album rendered inside the shared `<PageShell>` — header actions collapse the three creation flows into a `SplitButton` and keep refresh as an independent action, each card surfaces a "使用中" badge driven by `useDeviceContext().currentDisplay`, sync results flow through `useToast()` instead of an inline floating banner, and the in-flight generation card sits inside a shell `<Card>`. `AppearanceChannelModal` is preserved (visually reworked to match shell tokens) so users can still pick a target channel from the gallery; USB / device-online / agent-appearance state is read entirely from `DeviceContextProvider` and applied through its `applyDesktopPet` wrapper. |
```

- [ ] **Step 2: Update the `AppearanceGallery.test.js` row**

Replace its Function blurb with:

```markdown
| `AppearanceGallery.test.js` | `test` | Static Node coverage that the gallery is rendered inside `<PageShell>` with refresh + `SplitButton` actions, the 3 creation labels live inside the SplitButton, USB / online / agent state comes from `useDeviceContext()` (no local polling), sync results go through `useToast()`, the in-flight task is wrapped in a shell `<Card>`, each card exposes the "使用中" badge + `is-active` border driven by `currentDisplay`, `onSetAsDesktopPet` is the rename of the old `onSelectChannel` callback, and `AppearanceChannelModal` is preserved with shell-token surface chrome plus the unchanged channel-selection + `ChannelSwitchConfirmModal` flow. |
```

- [ ] **Step 3: Update the Architecture summary (optional but recommended)**

In the same file, find the first paragraph after `## Architecture` and append one sentence:

```markdown
- Sync: if this folder changes, update this file immediately. Plan 3 (2026-05-26) migrated `AppearanceGallery.jsx` onto the shared shell — header chrome flows through `<PageShell>`, USB/online/agent state through `useDeviceContext()`, and sync notifications through `useToast()`.
```

(Adapt to whatever the current bullet looks like — the existing file already has a `Sync:` bullet; replace it with the version above.)

- [ ] **Step 4: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/.folder.md
git commit -m "docs(gallery): folder map 同步形象画廊 shell 迁移"
```

---

## Definition of Done for Plan 3

- [ ] `ref/src/AppearanceGallery.jsx` renders inside `<PageShell title="形象画廊" actions={[refresh, addAppearanceSplitButton]}>`; no `page-toolbar` / `page-hero` / `gallery-actions` divs remain in this file.
- [ ] All local USB / device-online / agent-detection `useEffect` polling loops are gone; the page reads `usb`, `deviceConnected`, `agentAppearanceMap`, `enabledAgents`, `agentOptions`, `currentDisplay`, and `applyDesktopPet` from `useDeviceContext()`.
- [ ] Inline-style `sync-notice` floating banner is gone; all sync feedback flows through `useToast().push({ tone, title, message?, ttl? })`.
- [ ] `RunningTaskCard` is wrapped in a shell `<Card>` and only renders when `taskRunning`.
- [ ] The bottom-of-file card component is renamed `AppearanceCard` and accepts `isActive` + `onSetAsDesktopPet`; a "使用中" badge appears on the card whose id matches `currentDisplay.appearance?.id`.
- [ ] A local `SplitButton` helper exists in `AppearanceGallery.jsx` (NOT in `shell/`) and houses the 3 creation entrypoints (`新建自定义形象`, `从 Codex 导入`, `从社区导入`). The primary label click triggers the wizard; the chevron opens the menu. Menu closes on outside click and Escape.
- [ ] `AppearanceChannelModal` is preserved end-to-end — same props, same channel-selection flow, same `ChannelSwitchConfirmModal` integration — with refreshed `.modal-card--channel` CSS tokens.
- [ ] `ref/src/AppearanceGallery.test.js` covers every assertion above and the existing behavioral guarantees still in scope (Codex / community import internals, gated apply, configured-channel badge, cached scan + force refresh).
- [ ] `node --test src/AppearanceGallery.test.js` passes.
- [ ] `node --test src/shell/*.test.js` passes (no shell file modified).
- [ ] `node --test src/DeviceDashboard.test.js src/ComponentCenter.test.js src/AppearanceDetail.test.js src/ProductExperience.test.js src/DeviceGuideModal.test.js src/DeviceSetup.test.js src/CustomAvatarWizard.test.js` passes.
- [ ] `node --test src/lib/*.test.js` passes.
- [ ] `ref/src/.folder.md` reflects the shell migration on the AppearanceGallery rows.
- [ ] `git diff --stat ref/src/shell/` is empty — Plan 3 did not modify any shell file.

---

## Self-Review (run by the writer, not by an agent)

**Spec coverage:**
- Spec § Plan 3 page IA (`<PageShell>` with refresh + `<SplitButton>`) → Task 6 ✓
- Spec § Plan 3 page IA (`<Card>{<RunningTaskCard/>}</Card>` conditional) → Task 6 + Task 7 ✓
- Spec § Plan 3 page IA (`<AppearanceCard isActive onSetAsDesktopPet onClick/>`) → Task 4 ✓
- Spec § Plan 3 page IA (`AppearanceChannelModal` preserved, visually reworked) → Task 8 ✓
- Spec § "关键决定: 3 创建按钮收成 SplitButton" → Tasks 5 + 6 ✓
- Spec § "关键决定: 画廊保留完整渠道选择能力" → Task 8 + Task 2’s `applyDesktopPet` wiring ✓
- Spec § "关键决定: 使用中徽章 via `useDeviceContext().currentDisplay.appearance?.id === row.id`" → Task 4 ✓
- Spec § "关键决定: 删除画廊的 inline-style `sync-notice`，走 toast" → Task 3 ✓
- Spec § "刷新按钮独立" (画廊/组件中心刷新独立) → Task 6 (refresh stays as its own PageShell action, NOT inside SplitButton) ✓
- Spec § Out of Scope (`applyDesktopPetAssignment` 不重写) → Task 2 routes through `applyDesktopPet` wrapper from context, which calls the unchanged lib function ✓
- Spec § Out of Scope (持久化 schema 不变) → `agentAppearanceMap` and `enabledAgents` keys are still managed by `lib/agent-appearance-config.js` via the context; no new keys introduced ✓
- Spec § "shell API frozen post Plan 1" → Task 9 Step 2 enforces with `git diff --stat ref/src/shell/` ✓
- Spec § "保留 message-banner" → the inline error banner (`{error && <div className="message-banner message-banner--error">`) inside the gallery body is preserved in Task 6’s render tree (it’s a form-adjacent error, not a transient toast) ✓
- Spec § Out of Scope (CSS 旧 class 短期共存) → `.gallery-actions` block intentionally NOT deleted in Task 5; only new `.split-button` / `.appearance-card.is-active` / `.appearance-card__badge--active` rules added ✓
- Spec § "Plan 2/3/4 并行执行，工作互不耦合" → Plan 3 modifies only `AppearanceGallery.jsx`, its test, `styles.css` (only adds new selectors), and `.folder.md` ✓

**Placeholder scan:** None. Every step contains concrete code, exact file paths, exact commands, exact commit messages, and explicit expected output.

**Type consistency cross-check:**
- `useDeviceContext()` field names used in the new gallery (`usb`, `deviceConnected`, `agentAppearanceMap`, `enabledAgents`, `agentOptions`, `currentDisplay`, `applyDesktopPet`) match the Plan 1 Task 5 implementation byte-for-byte.
- `useToast().push(...)` shape (`{ tone, title, message?, ttl?, action? }`) matches Plan 1 Task 4’s implementation.
- `applyDesktopPet(agentId, appearance, { onProgress })` signature matches Plan 1 Task 5’s wrapper (it ignores `currentAppearanceId` / `agentAppearanceMap` / `agentOptions` / `boardDeviceId` / `deviceOnline` arguments — the wrapper injects them).
- `AppearanceCard` props (`row, isActive, disabled, configuredAgentIds, agentOptions, isSyncing, usbConnected, onOpenDetail, onSetAsDesktopPet`) match between Task 4’s definition and Task 6’s call site.
- `SplitButton` props (`label, primaryAction, items, disabled`) match between Task 5’s definition and Task 6’s call site. Each `item` carries `{ id, label, icon, onClick }`.

**Known fragile area:** Task 2 deletes the local agent-detection effect, which means `agentOptions` now comes entirely from `useDeviceContext()`. Plan 1 Task 5 loads `agentOptions` once on mount via `detect_local_agents` and exposes a `refresh()` method. The gallery’s old behavior called `detect_local_agents` on every gallery mount; with the new shape, the agent list is fetched once at App startup. This is a strict improvement in terms of duplicated polling, but if a user plugs in a new agent (e.g. installs Codex CLI) while the gallery is open, they would need to click somewhere that triggers `refresh()` to see it. This is acceptable because the dashboard’s OTA-friendly flows already require a manual refresh, and a future shell amendment can add a `useEffect` polling loop inside `DeviceContextProvider` if needed — out of scope for Plan 3.

**Known acceptable code-smell:** The `usbConnected` prop on `AppearanceCard` is named after the old local boolean rather than the new context field — kept for minimal blast radius (the prop is only used for the tooltip wording). A future cleanup can rename it to `usb` and pass `usb.connected` through, but this plan deliberately does not touch it to keep the diff focused.
