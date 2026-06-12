# Plan 1: Shared Shell Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the foundational app-level shell (`PageShell`, `Card`, `ContextRail`, `ToastStack` + `useToast`, `DeviceContextProvider`) and integrate it into `App.jsx`. After this plan, the existing pages still work unchanged; Plans 2/3/4 migrate them onto the new shell.

**Architecture:** Add a new `ref/src/shell/` folder containing the 5 shell pieces. `App.jsx` wraps everything in `DeviceContextProvider` + `ToastProvider`, adds `ContextRail` to the sidebar. Existing pages (`DeviceDashboard`, `AppearanceGallery`, `ComponentCenter`) are NOT modified in this plan; they continue to use their own internal state and toasts. This lets Plan 1 land independently and lets the parallel Plans 2/3/4 migrate one page at a time without breaking the others.

**Tech Stack:** React 18, lucide-react, vanilla CSS (existing `--accent`/`--line`/`--radius-*` tokens in `styles.css`), `node:test` for tests (static source analysis + pure-function runtime where possible).

**Spec reference:** `docs/superpowers/specs/2026-05-26-pet-manager-layout-redesign-design.md` — sections "Architecture: Shared Shell" and "Out of Scope".

---

## File Structure

**New files** (all under `ref/src/shell/`):

| Path | Responsibility |
|------|----------------|
| `ref/src/shell/.folder.md` | Folder map header (follow project pattern) |
| `ref/src/shell/PageShell.jsx` | `<PageShell title subtitle actions help>{children}</PageShell>` |
| `ref/src/shell/PageShell.test.js` | Static source assertions |
| `ref/src/shell/Card.jsx` | `<Card>` + `<Card.Collapsible>` |
| `ref/src/shell/Card.test.js` | Static source assertions |
| `ref/src/shell/ToastStack.jsx` | `<ToastProvider>`, `useToast()`, `<ToastStack>` (presentational) |
| `ref/src/shell/ToastStack.test.js` | Runtime queue test + static assertions |
| `ref/src/shell/DeviceContext.jsx` | `<DeviceContextProvider>`, `useDeviceContext()` + exported pure helpers |
| `ref/src/shell/DeviceContext.test.js` | Runtime tests for pure helpers + static assertions for provider |
| `ref/src/shell/ContextRail.jsx` | Sidebar bottom rail (3 lines + unbound state) |
| `ref/src/shell/ContextRail.test.js` | Static source assertions |

**Modified files:**

| Path | What changes |
|------|--------------|
| `ref/src/App.jsx` | Wrap tree in `<DeviceContextProvider>` + `<ToastProvider>`; render `<ContextRail/>` in sidebar; migrate existing inline `ToastStack` to use `useToast()` |
| `ref/src/styles.css` | Add `/* === shell === */` section after `:root` block: classes for `.page-shell`, `.page-shell__header`, `.card`, `.card--collapsible`, `.context-rail`, `.toast-stack` (refining existing class) |
| `ref/src/.folder.md` | Add entry for the new `shell/` subfolder under the Files table |
| `ref/src/.folder.md` (top) | Update Sync line / nothing structural |

**No changes** (explicitly out of scope for Plan 1): `DeviceDashboard.jsx`, `DeviceSetup.jsx`, `AppearanceGallery.jsx`, `ComponentCenter.jsx`, `CustomAvatarWizard.jsx`, any Tauri Rust file, any `lib/*.js`.

---

## Conventions used in this plan

- **All commands assume `cd /path/to/claw-pet-manager/ref`** unless otherwise noted.
- **Test runner**: `node --test src/shell/<file>.test.js` (the project has no `test` npm script — invoke `node --test` directly).
- **Existing test style** is static source analysis: read `.jsx` as text with `readFileSync`, assert with `node:assert/strict`. Examples in `src/DeviceDashboard.test.js`. We follow this pattern except where pure functions allow real runtime tests.
- **Commit style**: conventional, Chinese OK, no `--no-verify`. Pattern: `feat(shell): <one-line summary>`.

---

## Task 1: Scaffold the `shell/` folder

**Files:**
- Create: `ref/src/shell/.folder.md`
- Modify: `ref/src/.folder.md` (add shell row to Files table)

- [ ] **Step 1: Create the shell folder marker**

Create `ref/src/shell/.folder.md` with this content:

```markdown
# Folder Plan: ref/src/shell

## Architecture
- Scope: ref/src/shell
- Function: Cross-page shell primitives that compose every page in the manager — PageShell (header + actions + help), Card (with Collapsible variant), ToastStack (provider + hook + presentational stack), DeviceContextProvider (single source of polling for USB/online/binding/appearances/agents), ContextRail (sidebar bottom rail showing the device/appearance/component triad).
- Sync: if this folder changes, update this file immediately.
- Reuse: consumed by `App.jsx` (provider wiring + sidebar mount), and by every page in `ref/src/*.jsx` after Plans 2/3/4 migrate them. Pure derivations like `deriveCurrentDisplay` are unit-tested via `node:test`.

## Files
| File | Pos | Function |
|---|---|---|
| `PageShell.jsx` | `component` | Page-level wrapper that renders title/subtitle/right-side actions and an optional `?` help button, replacing scattered `page-hero` / `page-toolbar` / `component-store-hero` patterns |
| `PageShell.test.js` | `test` | Static Node coverage that the PageShell renders title/subtitle/actions/help when provided and exposes the documented prop names |
| `Card.jsx` | `component` | Section card with optional title/subtitle/actions, plus `Card.Collapsible` for collapsible cards used by the device dashboard's voice assistant |
| `Card.test.js` | `test` | Static Node coverage that Card uses the documented class names and Card.Collapsible toggles open/closed state |
| `ToastStack.jsx` | `component` | App-level toast queue: exports `<ToastProvider>` (context), `useToast()` (`{ push, dismiss }` hook), and `<ToastStack>` (presentational rendering anchored bottom-center) |
| `ToastStack.test.js` | `test` | Runtime Node test that useToast push/dismiss mutate the queue and auto-dismiss after timeout, plus static check that ToastStack reads from context |
| `DeviceContext.jsx` | `component` | Single source for binding/USB/deviceOnline/appearances/agentAppearanceMap/enabledAgents/agentOptions polling, exposes `useDeviceContext()` and pure helper `deriveCurrentDisplay` for tests |
| `DeviceContext.test.js` | `test` | Runtime Node coverage for `deriveCurrentDisplay` across bound/unbound/stale-channel cases + static check that Provider wires the documented context value shape |
| `ContextRail.jsx` | `component` | Sidebar bottom rail showing device chip + current appearance thumb + current component, collapsing to a single "+ 绑定设备" button when no binding |
| `ContextRail.test.js` | `test` | Static Node coverage that ContextRail renders the 3-row triad when bound, single CTA when unbound, and each row triggers the documented navigation callback |
```

- [ ] **Step 2: Append shell folder to parent `.folder.md`**

Open `ref/src/.folder.md` and find the Files table. Add this row at the END of the table (before the next markdown section, immediately after the last `lib/...` row):

```markdown
| `shell/` | `folder` | Cross-page shell primitives — see `shell/.folder.md` |
```

- [ ] **Step 3: Verify and commit**

Run:
```bash
cd /path/to/claw-pet-manager/ref
ls src/shell/
```
Expected: `.folder.md`

Commit:
```bash
cd /path/to/claw-pet-manager
git add ref/src/shell/.folder.md ref/src/.folder.md
git commit -m "feat(shell): 初始化 ref/src/shell 目录与 folder map"
```

---

## Task 2: PageShell component

**Files:**
- Create: `ref/src/shell/PageShell.jsx`
- Create: `ref/src/shell/PageShell.test.js`
- Modify: `ref/src/styles.css` (add `.page-shell` block)

- [ ] **Step 1: Write the failing test**

Create `ref/src/shell/PageShell.test.js`:

```javascript
/**
 * [Input] Read PageShell.jsx source.
 * [Output] Static Node coverage that PageShell renders title, optional subtitle/actions/help with documented class names and prop signature.
 * [Pos] test node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "PageShell.jsx"), "utf8");

test("PageShell exports a default React component", () => {
  assert.match(source, /export default function PageShell\s*\(/);
});

test("PageShell accepts the documented props", () => {
  for (const prop of ["title", "subtitle", "actions", "help", "children"]) {
    assert.match(source, new RegExp(`\\b${prop}\\b`), `expected prop ${prop} in PageShell`);
  }
});

test("PageShell uses the documented class hierarchy", () => {
  assert.match(source, /className="page-shell"/);
  assert.match(source, /className="page-shell__header"/);
  assert.match(source, /className="page-shell__title"/);
});

test("PageShell only renders subtitle when provided", () => {
  // Conditional render guard — keeps the header tight when subtitle is omitted.
  assert.match(source, /subtitle\s*&&/);
});

test("PageShell only renders the help icon when help is passed", () => {
  assert.match(source, /help\s*&&/);
  assert.match(source, /HelpCircle/);
});

test("PageShell renders the actions slot at the top-right", () => {
  assert.match(source, /className="page-shell__actions"/);
  // actions is rendered as-is (caller passes a node or array)
  assert.match(source, /\{actions\}/);
});
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/PageShell.test.js
```
Expected: ALL FAIL (file `PageShell.jsx` does not exist). The Node error is `ENOENT`.

- [ ] **Step 3: Implement PageShell**

Create `ref/src/shell/PageShell.jsx`:

```jsx
/**
 * [Input] Page-level title/subtitle/actions/help props plus children.
 * [Output] Unified page header + content wrapper used by every top-level page; replaces page-hero / page-toolbar / component-store-hero patterns.
 * [Pos] component node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import React from "react";
import { HelpCircle } from "lucide-react";

export default function PageShell({ title, subtitle, actions, help, children }) {
  return (
    <div className="page page-shell">
      <header className="page-shell__header">
        <div className="page-shell__title-block">
          <h1 className="page-shell__title">{title}</h1>
          {subtitle && <p className="page-shell__subtitle">{subtitle}</p>}
        </div>
        <div className="page-shell__trailing">
          {actions && <div className="page-shell__actions">{actions}</div>}
          {help && (
            <button
              type="button"
              className="icon-btn page-shell__help"
              onClick={help}
              aria-label="查看页面使用指南"
              title="查看页面使用指南"
            >
              <HelpCircle size={16} />
            </button>
          )}
        </div>
      </header>
      <div className="page-shell__body">{children}</div>
    </div>
  );
}
```

- [ ] **Step 4: Add CSS for PageShell**

Open `ref/src/styles.css`. Find the line `--content-max-width: 1080px;` near the top — that closes the `:root` block. After the `:root { ... }` closing brace and before the universal `* { box-sizing }` reset, insert this new section:

```css
/* === shell ============================================================ */

.page-shell {
  display: flex;
  flex-direction: column;
  gap: 16px;
  padding: 24px 28px 32px;
  max-width: var(--content-max-width);
  width: 100%;
  margin: 0 auto;
}

.page-shell__header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 24px;
}

.page-shell__title-block {
  display: flex;
  flex-direction: column;
  gap: 4px;
  min-width: 0;
}

.page-shell__title {
  margin: 0;
  font-size: 20px;
  font-weight: 600;
  color: var(--text);
}

.page-shell__subtitle {
  margin: 0;
  font-size: 13px;
  color: var(--text-muted);
  line-height: 1.5;
}

.page-shell__trailing {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}

.page-shell__actions {
  display: flex;
  align-items: center;
  gap: 8px;
}

.page-shell__help {
  width: 28px;
  height: 28px;
  border-radius: var(--radius-sm);
  color: var(--text-muted);
  display: inline-flex;
  align-items: center;
  justify-content: center;
}

.page-shell__help:hover {
  background: var(--surface-muted);
  color: var(--text);
}

.page-shell__body {
  display: flex;
  flex-direction: column;
  gap: 14px;
}
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/PageShell.test.js
```
Expected: ALL PASS (6 tests).

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/shell/PageShell.jsx ref/src/shell/PageShell.test.js ref/src/styles.css
git commit -m "feat(shell): add PageShell with title/subtitle/actions/help"
```

---

## Task 3: Card + Card.Collapsible

**Files:**
- Create: `ref/src/shell/Card.jsx`
- Create: `ref/src/shell/Card.test.js`
- Modify: `ref/src/styles.css` (extend shell section with `.card` rules)

- [ ] **Step 1: Write the failing test**

Create `ref/src/shell/Card.test.js`:

```javascript
/**
 * [Input] Read Card.jsx source.
 * [Output] Static Node coverage that Card renders title/subtitle/actions sections, and Card.Collapsible exposes open/close state with summary slot.
 * [Pos] test node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "Card.jsx"), "utf8");

test("Card exports a default React component", () => {
  assert.match(source, /export default function Card\s*\(/);
});

test("Card exposes Collapsible as a static property", () => {
  assert.match(source, /Card\.Collapsible\s*=/);
});

test("Card uses the documented class names", () => {
  assert.match(source, /className="card"/);
  assert.match(source, /className="card__header"/);
  assert.match(source, /className="card__title"/);
  assert.match(source, /className="card__body"/);
});

test("Card only renders header when title/subtitle/actions provided", () => {
  // The header should be conditionally rendered to keep cards tight when used as plain containers.
  assert.match(source, /title\s*\|\|\s*subtitle\s*\|\|\s*actions/);
});

test("Card does not accept a tone/variant prop (status lives in body banners/chips)", () => {
  // Explicit non-feature — guards against future drift.
  assert.doesNotMatch(source, /\btone\b/);
  assert.doesNotMatch(source, /\bvariant\b/);
});

test("Card.Collapsible accepts title, summary, defaultOpen, children", () => {
  for (const prop of ["title", "summary", "defaultOpen", "children"]) {
    assert.match(source, new RegExp(`\\b${prop}\\b`), `expected ${prop} in Collapsible`);
  }
});

test("Card.Collapsible uses useState for open state", () => {
  assert.match(source, /useState\(/);
});

test("Card.Collapsible uses the documented class names and chevron icon", () => {
  assert.match(source, /className="card card--collapsible/);
  assert.match(source, /ChevronDown|ChevronRight/);
});
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/Card.test.js
```
Expected: ALL FAIL (file missing).

- [ ] **Step 3: Implement Card and Card.Collapsible**

Create `ref/src/shell/Card.jsx`:

```jsx
/**
 * [Input] title/subtitle/actions/children props plus an opt-in Collapsible variant with summary.
 * [Output] Unified section card replacing panel-card / component-store-section / component-tool-card; Collapsible variant powers the device dashboard's "voice assistant" section.
 * [Pos] component node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import React, { useState } from "react";
import { ChevronDown } from "lucide-react";

export default function Card({ title, subtitle, actions, children, className = "" }) {
  const showHeader = title || subtitle || actions;
  return (
    <section className={`card ${className}`.trim()}>
      {showHeader && (
        <header className="card__header">
          <div className="card__title-block">
            {title && <h2 className="card__title">{title}</h2>}
            {subtitle && <p className="card__subtitle">{subtitle}</p>}
          </div>
          {actions && <div className="card__actions">{actions}</div>}
        </header>
      )}
      <div className="card__body">{children}</div>
    </section>
  );
}

function CardCollapsible({ title, summary, defaultOpen = false, children }) {
  const [open, setOpen] = useState(defaultOpen);
  return (
    <section className={`card card--collapsible${open ? " is-open" : ""}`}>
      <button
        type="button"
        className="card__header card__header--toggle"
        onClick={() => setOpen((prev) => !prev)}
        aria-expanded={open}
      >
        <div className="card__title-block">
          <h2 className="card__title">{title}</h2>
          {!open && summary && <p className="card__subtitle">{summary}</p>}
        </div>
        <ChevronDown
          size={16}
          className={`card__chevron${open ? " is-open" : ""}`}
          aria-hidden="true"
        />
      </button>
      {open && <div className="card__body">{children}</div>}
    </section>
  );
}

Card.Collapsible = CardCollapsible;
```

- [ ] **Step 4: Add CSS for Card and Card.Collapsible**

In `ref/src/styles.css`, find the `/* === shell ===` section added in Task 2. Append at the end of that section (still before the `* { box-sizing }` reset or whatever boundary you've identified):

```css
.card {
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: var(--radius-lg);
  box-shadow: var(--shadow-xs);
  display: flex;
  flex-direction: column;
}

.card__header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 16px;
  padding: 14px 18px;
  border-bottom: 1px solid var(--line-soft);
}

.card__header--toggle {
  width: 100%;
  border: none;
  background: transparent;
  padding: 14px 18px;
  text-align: left;
  border-bottom: 1px solid transparent;
  cursor: pointer;
}

.card--collapsible.is-open .card__header--toggle {
  border-bottom-color: var(--line-soft);
}

.card__title-block {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.card__title {
  margin: 0;
  font-size: 14px;
  font-weight: 600;
  color: var(--text);
}

.card__subtitle {
  margin: 0;
  font-size: 12px;
  color: var(--text-muted);
  line-height: 1.5;
}

.card__actions {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}

.card__body {
  padding: 14px 18px 18px;
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.card__chevron {
  color: var(--text-muted);
  transition: transform 160ms ease;
}

.card__chevron.is-open {
  transform: rotate(180deg);
}
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/Card.test.js
```
Expected: ALL PASS (8 tests).

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/shell/Card.jsx ref/src/shell/Card.test.js ref/src/styles.css
git commit -m "feat(shell): add Card and Card.Collapsible"
```

---

## Task 4: ToastStack — provider, hook, presentational stack

**Files:**
- Create: `ref/src/shell/ToastStack.jsx`
- Create: `ref/src/shell/ToastStack.test.js`
- Modify: `ref/src/styles.css` (replace/extend `.toast-stack`)

Note: existing `.toast-stack` CSS may already exist in `styles.css` (used by current inline `ToastStack` in App.jsx). Keep its visual rules; we are only refactoring the React side and slightly extending CSS.

- [ ] **Step 1: Write the failing test**

Create `ref/src/shell/ToastStack.test.js`:

```javascript
/**
 * [Input] Read ToastStack.jsx source + invoke useToast in a runtime check.
 * [Output] Static + runtime Node coverage that ToastProvider exposes useToast with push/dismiss, queues multiple toasts, auto-dismisses after ttl, and ToastStack renders from context.
 * [Pos] test node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "ToastStack.jsx"), "utf8");

test("ToastStack source exports ToastProvider, useToast, and a default ToastStack", () => {
  assert.match(source, /export function ToastProvider\s*\(/);
  assert.match(source, /export function useToast\s*\(/);
  assert.match(source, /export default function ToastStack\s*\(/);
});

test("useToast returns push and dismiss methods", () => {
  assert.match(source, /push\s*[:,(]/);
  assert.match(source, /dismiss\s*[:,(]/);
});

test("Toasts auto-dismiss via setTimeout with a configurable ttl", () => {
  assert.match(source, /setTimeout/);
  assert.match(source, /ttl/);
});

test("ToastStack renders queue items with tone, title, optional message and action", () => {
  for (const key of ["tone", "title", "message", "action"]) {
    assert.match(source, new RegExp(`\\b${key}\\b`), `toast item should support ${key}`);
  }
});

test("ToastStack uses the documented presentational classes", () => {
  assert.match(source, /className="toast-stack"/);
  assert.match(source, /toast--/);
});
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/ToastStack.test.js
```
Expected: ALL FAIL (file missing).

- [ ] **Step 3: Implement ToastProvider, useToast, ToastStack**

Create `ref/src/shell/ToastStack.jsx`:

```jsx
/**
 * [Input] Children tree consuming useToast; toast push payloads.
 * [Output] App-level toast queue with ToastProvider/useToast hook and a bottom-anchored ToastStack rendering tone/title/message/action items with auto-dismiss; replaces App.jsx inline ToastStack and AppearanceGallery inline-style sync-notice.
 * [Pos] component node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "react";
import { CheckCircle, AlertCircle, Info, X } from "lucide-react";

const ToastContext = createContext(null);

const DEFAULT_TTL = 4000;
const ICONS = {
  success: CheckCircle,
  error: AlertCircle,
  warning: AlertCircle,
  info: Info,
};

let nextToastId = 1;

export function ToastProvider({ children }) {
  const [items, setItems] = useState([]);
  const timersRef = useRef(new Map());

  const dismiss = useCallback((id) => {
    setItems((prev) => prev.filter((item) => item.id !== id));
    const timer = timersRef.current.get(id);
    if (timer) {
      clearTimeout(timer);
      timersRef.current.delete(id);
    }
  }, []);

  const push = useCallback(
    (toast) => {
      const id = nextToastId++;
      const ttl = typeof toast.ttl === "number" ? toast.ttl : DEFAULT_TTL;
      setItems((prev) => [...prev, { ...toast, id }]);
      if (ttl > 0) {
        const timer = setTimeout(() => dismiss(id), ttl);
        timersRef.current.set(id, timer);
      }
      return id;
    },
    [dismiss],
  );

  useEffect(() => {
    const timers = timersRef.current;
    return () => {
      for (const timer of timers.values()) clearTimeout(timer);
      timers.clear();
    };
  }, []);

  const value = useMemo(() => ({ push, dismiss, items }), [push, dismiss, items]);
  return <ToastContext.Provider value={value}>{children}</ToastContext.Provider>;
}

export function useToast() {
  const ctx = useContext(ToastContext);
  if (!ctx) throw new Error("useToast must be used inside <ToastProvider>");
  return ctx;
}

export default function ToastStack() {
  const { items, dismiss } = useToast();
  if (items.length === 0) return null;
  return (
    <div className="toast-stack">
      {items.map(({ id, tone = "info", title, message, action }) => {
        const Icon = ICONS[tone] || Info;
        return (
          <div key={id} className={`toast toast--${tone}`} role="status" aria-live="polite">
            <div className="toast__head">
              <Icon size={16} />
              <span className="toast__title">{title}</span>
              <button
                type="button"
                className="icon-btn"
                onClick={() => dismiss(id)}
                aria-label="关闭通知"
              >
                <X size={14} />
              </button>
            </div>
            {message && (
              <div className="muted small" style={{ whiteSpace: "pre-wrap" }}>
                {message}
              </div>
            )}
            {action && (
              <div className="toast__actions">
                <button
                  className="btn-primary btn-sm"
                  type="button"
                  onClick={() => {
                    action.onClick?.();
                    dismiss(id);
                  }}
                >
                  {action.label}
                </button>
              </div>
            )}
          </div>
        );
      })}
    </div>
  );
}
```

- [ ] **Step 4: Add/extend CSS for toast-stack**

In `ref/src/styles.css`, search for the existing `.toast-stack` class. If it already exists (it does — used by current App.jsx ToastStack), leave its visual rules alone; the new ToastStack uses the same outer class so it'll look identical. If there is no `.toast__actions` rule, append into the `/* === shell ===` block:

```css
.toast__actions {
  display: flex;
  justify-content: flex-end;
  margin-top: 6px;
}
```

If `.toast-stack` does NOT already exist, add this full block to the shell section instead:

```css
.toast-stack {
  position: fixed;
  bottom: 24px;
  left: 50%;
  transform: translateX(-50%);
  z-index: 9999;
  display: flex;
  flex-direction: column;
  gap: 8px;
  max-width: 90vw;
}

.toast {
  background: var(--surface);
  border: 1px solid var(--line);
  border-radius: var(--radius-md);
  padding: 10px 14px;
  box-shadow: var(--shadow-md);
  min-width: 280px;
  font-size: 13px;
}

.toast--success { border-color: var(--success); }
.toast--error   { border-color: var(--danger); }
.toast--warning { border-color: var(--warning); }
.toast--info    { border-color: var(--info); }

.toast__head {
  display: flex;
  align-items: center;
  gap: 8px;
}

.toast__title { flex: 1; font-weight: 600; }

.toast__actions {
  display: flex;
  justify-content: flex-end;
  margin-top: 6px;
}
```

To check whether `.toast-stack` already exists:
```bash
cd /path/to/claw-pet-manager/ref
grep -n "\.toast-stack" src/styles.css
```

- [ ] **Step 5: Run static + runtime tests**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/ToastStack.test.js
```
Expected: ALL PASS (5 tests).

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/shell/ToastStack.jsx ref/src/shell/ToastStack.test.js ref/src/styles.css
git commit -m "feat(shell): add ToastProvider + useToast + ToastStack"
```

---

## Task 5: DeviceContextProvider

This is the heaviest task. It centralizes all polling logic that today is duplicated across DeviceDashboard, AppearanceGallery, and ComponentCenter. We expose a pure `deriveCurrentDisplay` helper that is fully unit-testable, and write static checks for the Provider shell.

**Files:**
- Create: `ref/src/shell/DeviceContext.jsx`
- Create: `ref/src/shell/DeviceContext.test.js`

- [ ] **Step 1: Write the failing tests**

Create `ref/src/shell/DeviceContext.test.js`:

```javascript
/**
 * [Input] Read DeviceContext.jsx source + runtime-import deriveCurrentDisplay.
 * [Output] Static + runtime Node coverage that the provider exposes the documented context shape and the pure derivation reflects the active desktop assignment.
 * [Pos] test node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import { deriveCurrentDisplay } from "./DeviceContext.jsx";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "DeviceContext.jsx"), "utf8");

const APPEARANCES = [
  { id: "ap-a", name: "Terrier" },
  { id: "ap-b", name: "Westie" },
];
const AGENTS = [
  { id: "codex", label: "Codex", detected: true },
  { id: "claude-code", label: "Claude Code", detected: true },
];

test("deriveCurrentDisplay returns the active assignment with appearance + label", () => {
  const out = deriveCurrentDisplay(
    { codex: "ap-a" },
    new Set(["codex"]),
    APPEARANCES,
    AGENTS,
  );
  assert.equal(out.agentId, "codex");
  assert.equal(out.appearance?.id, "ap-a");
  assert.equal(out.channelLabel, "Codex");
});

test("deriveCurrentDisplay returns null appearance when map is empty", () => {
  const out = deriveCurrentDisplay({}, new Set(), APPEARANCES, AGENTS);
  assert.equal(out.agentId, "");
  assert.equal(out.appearance, null);
});

test("deriveCurrentDisplay falls back to the first mapped agent when no enabled set", () => {
  const out = deriveCurrentDisplay(
    { codex: "ap-b" },
    null,
    APPEARANCES,
    AGENTS,
  );
  assert.equal(out.agentId, "codex");
  assert.equal(out.appearance?.id, "ap-b");
});

test("provider source exposes useDeviceContext and DeviceContextProvider", () => {
  assert.match(source, /export function DeviceContextProvider\s*\(/);
  assert.match(source, /export function useDeviceContext\s*\(/);
});

test("provider centralizes the documented polling and bridge invocations", () => {
  // No new Tauri commands — strictly re-uses existing ones from the dashboards.
  for (const command of [
    "usb_get_status",
    "usb_scan_devices",
    "usb_connect",
    "check_device_availability",
    "load_bridge_profile",
    "load_device_bindings",
    "detect_local_agents",
  ]) {
    assert.match(source, new RegExp(`["']${command}["']`), `expected provider to invoke ${command}`);
  }
});

test("provider exposes the documented context shape fields", () => {
  for (const field of [
    "binding",
    "usb",
    "deviceOnline",
    "onlineBoardDeviceId",
    "deviceConnected",
    "appearances",
    "agentAppearanceMap",
    "enabledAgents",
    "agentOptions",
    "currentDisplay",
    "currentComponent",
    "applyDesktopPet",
    "refresh",
  ]) {
    assert.match(source, new RegExp(`\\b${field}\\b`), `expected context field ${field}`);
  }
});

test("provider reuses applyDesktopPetAssignment from lib (does not re-implement)", () => {
  assert.match(source, /from\s+["'][^"']*desktop-pet-assignment[^"']*["']/);
  assert.match(source, /applyDesktopPetAssignment\(/);
});

test("provider reads currentComponent from a stable localStorage key with null fallback", () => {
  assert.match(source, /pet-manager:active-component/);
});
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/DeviceContext.test.js
```
Expected: ALL FAIL (file missing → ENOENT on import).

- [ ] **Step 3: Implement DeviceContextProvider + deriveCurrentDisplay**

Create `ref/src/shell/DeviceContext.jsx`:

```jsx
/**
 * [Input] children tree consuming useDeviceContext; Tauri invoke for USB/online/bindings/agents; lib helpers for appearance/agent storage.
 * [Output] Single source of polling and derived state (binding, USB, deviceOnline, appearances, agentAppearanceMap, enabledAgents, agentOptions, currentDisplay, currentComponent) replacing duplicated useEffects across the 3 pages.
 * [Pos] component node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import {
  FIXED_AGENT_OPTIONS,
  activeDesktopAssignment,
  appearanceById,
  channelLabelForId,
  loadAgentAppearanceMap,
  loadEnabledAgents,
  normalizeDetectedAgents,
  saveAgentAppearanceMap,
  saveEnabledAgents,
  assignedAgentIds,
} from "../lib/agent-appearance-config.js";
import { applyDesktopPetAssignment as libApplyDesktopPetAssignment } from "../lib/desktop-pet-assignment.js";
import { resolveOnlineBoardDeviceId } from "../lib/device-availability.js";
import { listAppearances } from "../lib/appearance-store.js";

const ACTIVE_COMPONENT_STORAGE_KEY = "pet-manager:active-component";

const DeviceContext = createContext(null);

/**
 * Pure helper for unit tests. The reason this exists separately from the
 * hook value: the actual Provider mixes polling + storage, which is painful
 * to test without a DOM. This function takes the inputs the Provider would
 * have computed and produces the same `currentDisplay` shape consumers read.
 */
export function deriveCurrentDisplay(agentAppearanceMap, enabledAgents, appearances, agentOptions) {
  const active = activeDesktopAssignment(agentAppearanceMap, enabledAgents);
  return {
    agentId: active.agentId,
    appearance: appearanceById(appearances, active.appearanceId),
    channelLabel: active.agentId ? channelLabelForId(agentOptions, active.agentId) : "",
  };
}

function readActiveComponent() {
  try {
    const raw = localStorage.getItem(ACTIVE_COMPONENT_STORAGE_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    if (parsed && typeof parsed === "object" && parsed.id) return parsed;
  } catch {
    // ignore
  }
  return null;
}

export function DeviceContextProvider({ binding: bindingProp, onBindingChange, children }) {
  const [binding, setBindingState] = useState(bindingProp || null);
  useEffect(() => setBindingState(bindingProp || null), [bindingProp]);

  const [usb, setUsb] = useState({ connected: false, portName: "", boardDeviceId: "" });
  const [deviceOnline, setDeviceOnline] = useState(false);
  const [onlineBoardDeviceId, setOnlineBoardDeviceId] = useState("");
  const [appearances, setAppearances] = useState([]);
  const [agentAppearanceMap, setAgentAppearanceMap] = useState({});
  const [enabledAgents, setEnabledAgents] = useState(new Set());
  const [agentOptions, setAgentOptions] = useState(() =>
    FIXED_AGENT_OPTIONS.map((agent) => ({ ...agent, detected: false })),
  );
  const [currentComponent, setCurrentComponent] = useState(() => readActiveComponent());

  // --- USB poll (3s) with auto-connect attempt ---
  useEffect(() => {
    let cancelled = false;
    let connecting = false;
    const check = async () => {
      try {
        const status = await invoke("usb_get_status");
        if (cancelled) return;
        setUsb({
          connected: !!status?.connected,
          portName: status?.portName || "",
          boardDeviceId: status?.connected ? status?.boardDeviceId || "" : "",
        });
        if (!status?.connected && !connecting) {
          connecting = true;
          try {
            const devices = await invoke("usb_scan_devices");
            if (!cancelled && devices && devices.length > 0) {
              await invoke("usb_connect", { portName: devices[0].portName });
              const updated = await invoke("usb_get_status");
              if (!cancelled) {
                setUsb({
                  connected: !!updated?.connected,
                  portName: updated?.portName || "",
                  boardDeviceId: updated?.connected ? updated?.boardDeviceId || "" : "",
                });
              }
            }
          } catch (err) {
            console.warn("[DeviceContext] usb auto-connect failed", err);
          } finally {
            connecting = false;
          }
        }
      } catch (err) {
        console.warn("[DeviceContext] usb_get_status failed", err);
      }
    };
    check();
    const id = setInterval(check, 3000);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, []);

  // --- Device-online poll (5s, only when USB disconnected) ---
  useEffect(() => {
    if (usb.connected) {
      setDeviceOnline(true);
      setOnlineBoardDeviceId(usb.boardDeviceId || binding?.boardDeviceId || "");
      return undefined;
    }
    if (!binding) {
      setDeviceOnline(false);
      setOnlineBoardDeviceId("");
      return undefined;
    }
    let cancelled = false;
    const poll = () => {
      invoke("check_device_availability")
        .then((res) => {
          if (cancelled) return;
          const devices = res?.devices || {};
          const id = resolveOnlineBoardDeviceId(devices, binding);
          setDeviceOnline(Boolean(id));
          setOnlineBoardDeviceId(id);
        })
        .catch(() => {
          if (!cancelled) {
            setDeviceOnline(false);
            setOnlineBoardDeviceId("");
          }
        });
    };
    poll();
    const id = setInterval(poll, 5000);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, [binding, usb.connected, usb.boardDeviceId]);

  // --- Initial: load bridge profile, detect agents, list appearances ---
  const loadAppearances = useCallback(async () => {
    try {
      const records = await listAppearances();
      setAppearances(records);
      const map = loadAgentAppearanceMap(records);
      setAgentAppearanceMap(map);
      const enabled = loadEnabledAgents() || new Set();
      setEnabledAgents(enabled);
      return records;
    } catch (err) {
      console.warn("[DeviceContext] listAppearances failed", err);
      return [];
    }
  }, []);

  const detectAgents = useCallback(async () => {
    try {
      const res = await invoke("detect_local_agents");
      const next = normalizeDetectedAgents(res?.agents || []);
      setAgentOptions(next);
    } catch (err) {
      console.warn("[DeviceContext] detect_local_agents failed", err);
    }
  }, []);

  useEffect(() => {
    invoke("load_bridge_profile").catch(() => null);
    invoke("load_device_bindings").catch(() => null);
    loadAppearances();
    detectAgents();
  }, [loadAppearances, detectAgents]);

  // --- Cross-tab + same-tab updates of active component ---
  useEffect(() => {
    const handler = () => setCurrentComponent(readActiveComponent());
    window.addEventListener("storage", handler);
    return () => window.removeEventListener("storage", handler);
  }, []);

  const refresh = useCallback(async () => {
    await Promise.all([loadAppearances(), detectAgents()]);
  }, [loadAppearances, detectAgents]);

  const currentDisplay = useMemo(
    () => deriveCurrentDisplay(agentAppearanceMap, enabledAgents, appearances, agentOptions),
    [agentAppearanceMap, enabledAgents, appearances, agentOptions],
  );

  const deviceConnected = Boolean(usb.connected || deviceOnline);

  const applyDesktopPet = useCallback(
    async (agentId, appearance, options = {}) => {
      const { onProgress } = options;
      const currentAppearanceId = currentDisplay.appearance?.id || "";
      const { nextMap, notice } = await libApplyDesktopPetAssignment({
        invoke,
        listen,
        agentAppearanceMap,
        agentId,
        appearance,
        agentOptions,
        boardDeviceId: onlineBoardDeviceId || usb.boardDeviceId || binding?.boardDeviceId || "",
        currentAppearanceId,
        deviceOnline,
        onProgress,
      });
      setAgentAppearanceMap(nextMap);
      const enabled = new Set(assignedAgentIds(nextMap, agentId));
      setEnabledAgents(enabled);
      saveAgentAppearanceMap(nextMap);
      saveEnabledAgents(enabled);
      return { nextMap, notice };
    },
    [agentAppearanceMap, agentOptions, binding, currentDisplay, deviceOnline, onlineBoardDeviceId, usb.boardDeviceId],
  );

  const setBinding = useCallback(
    (next) => {
      setBindingState(next);
      onBindingChange?.(next);
    },
    [onBindingChange],
  );

  const value = useMemo(
    () => ({
      binding,
      setBinding,
      usb,
      deviceOnline,
      onlineBoardDeviceId,
      deviceConnected,
      appearances,
      agentAppearanceMap,
      enabledAgents,
      agentOptions,
      currentDisplay,
      currentComponent,
      applyDesktopPet,
      refresh,
    }),
    [
      binding,
      setBinding,
      usb,
      deviceOnline,
      onlineBoardDeviceId,
      deviceConnected,
      appearances,
      agentAppearanceMap,
      enabledAgents,
      agentOptions,
      currentDisplay,
      currentComponent,
      applyDesktopPet,
      refresh,
    ],
  );

  return <DeviceContext.Provider value={value}>{children}</DeviceContext.Provider>;
}

export function useDeviceContext() {
  const ctx = useContext(DeviceContext);
  if (!ctx) throw new Error("useDeviceContext must be used inside <DeviceContextProvider>");
  return ctx;
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/DeviceContext.test.js
```
Expected: ALL PASS (8 tests).

**Note:** the runtime tests for `deriveCurrentDisplay` will import `DeviceContext.jsx`. Node's test runner with `--experimental-vm-modules` is NOT needed because we are NOT executing any React-rendered code — we only import the pure exported helper. Verify the import succeeds.

If the import fails with "Cannot find module '@tauri-apps/api/core'" when running the test, this means the test runner tries to resolve transitive imports. To avoid this, the test only uses the **named export** `deriveCurrentDisplay`, which Node's ESM tree-shaking will NOT save you from — the file still loads. If this happens, restructure to put `deriveCurrentDisplay` into its own file `DeviceContext.pure.js` and import the pure file from both `DeviceContext.jsx` and the test. Add a step:

> If `node --test` fails with module-not-found on `@tauri-apps/api/core`, split: create `ref/src/shell/DeviceContext.pure.js` containing only `deriveCurrentDisplay` (and its dependencies on `lib/agent-appearance-config.js`), import that into both `DeviceContext.jsx` and the test, and adjust the test's import to `./DeviceContext.pure.js`.

- [ ] **Step 5: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/shell/DeviceContext.jsx ref/src/shell/DeviceContext.test.js
# Add DeviceContext.pure.js if you ended up creating it for testability.
git commit -m "feat(shell): add DeviceContextProvider + useDeviceContext + deriveCurrentDisplay"
```

---

## Task 6: ContextRail

**Files:**
- Create: `ref/src/shell/ContextRail.jsx`
- Create: `ref/src/shell/ContextRail.test.js`
- Modify: `ref/src/styles.css` (add `.context-rail` rules to shell section)

- [ ] **Step 1: Write the failing test**

Create `ref/src/shell/ContextRail.test.js`:

```javascript
/**
 * [Input] Read ContextRail.jsx source.
 * [Output] Static Node coverage that ContextRail renders the bound triad (device/appearance/component) with navigation callbacks, and collapses to a single bind CTA when no binding.
 * [Pos] test node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "ContextRail.jsx"), "utf8");

test("ContextRail exports a default React component", () => {
  assert.match(source, /export default function ContextRail\s*\(/);
});

test("ContextRail reads its data from useDeviceContext (single source)", () => {
  assert.match(source, /useDeviceContext\(/);
});

test("ContextRail accepts navigation callbacks for the three rows", () => {
  for (const cb of ["onOpenDevice", "onOpenAppearance", "onOpenComponent", "onStartBinding"]) {
    assert.match(source, new RegExp(`\\b${cb}\\b`), `expected ${cb} callback`);
  }
});

test("ContextRail uses the documented class names", () => {
  assert.match(source, /className="context-rail"/);
  assert.match(source, /context-rail__row/);
});

test("ContextRail collapses to a bind CTA when no binding", () => {
  // The branch must visibly include the bind label and call onStartBinding.
  assert.match(source, /绑定设备/);
});
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/ContextRail.test.js
```
Expected: ALL FAIL (file missing).

- [ ] **Step 3: Implement ContextRail**

Create `ref/src/shell/ContextRail.jsx`:

```jsx
/**
 * [Input] useDeviceContext for binding/usb/online/currentDisplay/currentComponent; navigation callbacks for the three rows + bind CTA.
 * [Output] Sidebar-bottom rail showing device chip + current appearance + current component, collapsing to a single bind CTA when no binding.
 * [Pos] component node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import React from "react";
import {
  Monitor,
  Wifi,
  WifiOff,
  Usb,
  Image as ImageIcon,
  Blocks,
  Plus,
} from "lucide-react";
import { useDeviceContext } from "./DeviceContext.jsx";

export default function ContextRail({
  onOpenDevice,
  onOpenAppearance,
  onOpenComponent,
  onStartBinding,
}) {
  const { binding, usb, deviceOnline, currentDisplay, currentComponent } = useDeviceContext();

  if (!binding) {
    return (
      <div className="context-rail context-rail--empty">
        <button type="button" className="context-rail__cta" onClick={onStartBinding}>
          <Plus size={14} />
          <span>绑定设备</span>
        </button>
      </div>
    );
  }

  const connectionIcon = usb.connected ? <Usb size={12} /> : deviceOnline ? <Wifi size={12} /> : <WifiOff size={12} />;
  const connectionLabel = usb.connected ? "USB" : deviceOnline ? "在线" : "离线";

  return (
    <div className="context-rail">
      <button type="button" className="context-rail__row" onClick={onOpenDevice} title="打开设备页">
        <Monitor size={14} />
        <span className="context-rail__primary">{binding.boardDeviceId}</span>
        <span className={`context-rail__chip context-rail__chip--${usb.connected ? "ok" : deviceOnline ? "ok" : "warn"}`}>
          {connectionIcon}
          {connectionLabel}
        </span>
      </button>

      <button type="button" className="context-rail__row" onClick={onOpenAppearance} title="打开形象画廊">
        <ImageIcon size={14} />
        <span className="context-rail__primary">
          {currentDisplay.appearance?.name || "未配置形象"}
        </span>
        {currentDisplay.channelLabel && (
          <span className="context-rail__muted">{currentDisplay.channelLabel}</span>
        )}
      </button>

      <button type="button" className="context-rail__row" onClick={onOpenComponent} title="打开组件中心">
        <Blocks size={14} />
        <span className="context-rail__primary">
          {currentComponent?.name || "未选择组件"}
        </span>
      </button>
    </div>
  );
}
```

- [ ] **Step 4: Add CSS for ContextRail**

In `ref/src/styles.css`, append to the `/* === shell ===` section:

```css
.context-rail {
  margin: 12px;
  padding: 10px;
  border: 1px solid var(--line);
  border-radius: var(--radius-md);
  background: var(--surface);
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.context-rail--empty {
  padding: 8px;
}

.context-rail__cta {
  width: 100%;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  padding: 8px 12px;
  border-radius: var(--radius-sm);
  color: var(--accent);
  background: var(--accent-soft);
  font-weight: 600;
}

.context-rail__row {
  display: grid;
  grid-template-columns: auto 1fr auto;
  align-items: center;
  gap: 8px;
  padding: 6px 8px;
  border-radius: var(--radius-sm);
  color: var(--text);
  text-align: left;
}

.context-rail__row:hover {
  background: var(--surface-muted);
}

.context-rail__primary {
  font-size: 12px;
  font-weight: 600;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  min-width: 0;
}

.context-rail__muted {
  font-size: 11px;
  color: var(--text-muted);
}

.context-rail__chip {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 2px 6px;
  border-radius: var(--radius-pill);
  font-size: 11px;
  font-weight: 600;
}

.context-rail__chip--ok { color: var(--success); background: var(--success-soft); }
.context-rail__chip--warn { color: var(--warning); background: var(--warning-soft); }
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/ContextRail.test.js
```
Expected: ALL PASS (5 tests).

- [ ] **Step 6: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/shell/ContextRail.jsx ref/src/shell/ContextRail.test.js ref/src/styles.css
git commit -m "feat(shell): add ContextRail sidebar bottom rail"
```

---

## Task 7: Integrate shell into App.jsx

This is the integration point. App.jsx currently:
1. Loads bindings and routes between `loading`/`setup`/`dashboard`/`gallery`/`wizard`/`detail`/`components`
2. Subscribes to generation-task and shows an inline `ToastStack`
3. Renders an `app-sidebar` with nav buttons

After this task:
- Tree is wrapped in `<DeviceContextProvider>` and `<ToastProvider>`
- The inline `ToastStack` (defined at bottom of App.jsx) is REMOVED — replaced by the shell ToastStack, mounted once inside the providers
- Sidebar gets `<ContextRail/>` at its bottom
- The generation-task subscription pushes to `useToast()` instead of `setToast()`
- App.jsx still owns binding loading and view routing; binding flows into the Provider via the `binding` prop

**Files:**
- Modify: `ref/src/App.jsx`
- Verify: existing tests under `ref/src/*.test.js` still pass (no behavior should regress)

- [ ] **Step 1: Edit App.jsx — add imports**

Open `ref/src/App.jsx`. At the top with the other imports (after `import ComponentCenter from "./ComponentCenter";`) add:

```jsx
import { DeviceContextProvider } from "./shell/DeviceContext.jsx";
import { ToastProvider, useToast } from "./shell/ToastStack.jsx";
import ToastStack from "./shell/ToastStack.jsx";
import ContextRail from "./shell/ContextRail.jsx";
```

- [ ] **Step 2: Remove the inline ToastStack definition**

Delete the entire `function ToastStack(...)` definition at the BOTTOM of App.jsx (the function previously starting around line 285 and ending around line 315). It will be replaced by the shell ToastStack imported above.

- [ ] **Step 3: Restructure App default export**

Replace the existing `export default function App() { ... }` body so the rendered tree is wrapped. Concretely, the **existing** body returns one of:
- `<div className="app-shell"><div className="auth-shell">...</div></div>` for loading
- `<div className="app-shell wizard-mode">...</div>` for setup
- `<div className="app-shell"><div className="app-layout">...sidebar+main...</div>{toast}</div>` for dashboard/gallery/etc.

Wrap the bound-or-setup return in providers and inject `<ContextRail/>` + the shell `<ToastStack/>`. The new body looks like (showing only the part that changes — keep all your existing useCallback / useEffect / state declarations):

```jsx
  // ... all existing state, callbacks, effects unchanged ...

  if (view === "loading") {
    return (
      <div className="app-shell">
        <div className="auth-shell">
          <div className="auth-card">
            <div className="auth-loading">
              <Loader2 size={18} className="spin" />
              <span>正在打开管理端…</span>
            </div>
          </div>
        </div>
      </div>
    );
  }

  // Both setup AND dashboard views share the same providers, so toast/context
  // survive across the setup → dashboard transition (e.g. a generation finishing
  // while the user is still on setup).
  return (
    <ToastProvider>
      <DeviceContextProvider
        binding={binding}
        onBindingChange={(next) => setBinding(next)}
      >
        <AppInner
          view={view}
          binding={binding}
          activeTab={activeTab}
          isDashboard={isDashboard}
          isSetup={isSetup}
          hasBinding={hasBinding}
          setView={setView}
          detailAppearanceId={detailAppearanceId}
          handleSetupComplete={handleSetupComplete}
          handleUnbind={handleUnbind}
          handleOpenGallery={handleOpenGallery}
          handleOpenComponents={handleOpenComponents}
          handleEnterWizard={handleEnterWizard}
          handleWizardExit={handleWizardExit}
          handleOpenDetail={handleOpenDetail}
          handleDetailBack={handleDetailBack}
          handleToastView={handleToastView}
          handleToastDismiss={handleToastDismiss}
        />
        <ToastStack />
      </DeviceContextProvider>
    </ToastProvider>
  );
}
```

Then add a new internal `AppInner` component below the default export that contains the actual sidebar+main rendering (the existing dashboard return branch), plus the migrated generation-task subscription. The reason we extract `AppInner`: hooks like `useToast()` must be called from inside the providers, and the existing App component is OUTSIDE the providers in the current structure.

Add this below the `export default function App` definition:

```jsx
function AppInner({
  view,
  binding,
  activeTab,
  isDashboard,
  isSetup,
  hasBinding,
  setView,
  detailAppearanceId,
  handleSetupComplete,
  handleUnbind,
  handleOpenGallery,
  handleOpenComponents,
  handleEnterWizard,
  handleWizardExit,
  handleOpenDetail,
  handleDetailBack,
}) {
  const { push } = useToast();
  const lastEpochRef = useRef(0);

  // Generation-task toasts go through the global queue now.
  useEffect(() => {
    return subscribeGenerationTask((s) => {
      if (s.completionEpoch <= lastEpochRef.current) return;
      if (s.status !== "completed" && s.status !== "failed") return;
      lastEpochRef.current = s.completionEpoch;
      push({
        tone: s.status === "completed" ? "success" : "error",
        title:
          s.status === "completed"
            ? `「${s.appearanceName}」生成完成`
            : `「${s.appearanceName}」生成失败`,
        message: s.status === "failed" ? s.error : "",
        ttl: 6000,
        action: s.appearanceId
          ? {
              label: "查看",
              onClick: () => {
                acknowledgeGenerationTask();
                handleOpenDetail(s.appearanceId);
              },
            }
          : null,
      });
      // Auto-ack on dismiss handled by ToastStack via onClick; for plain
      // dismiss without action button we ack on render below.
    });
  }, [push, handleOpenDetail]);

  if (isSetup) {
    return (
      <div className="app-shell wizard-mode">
        <div className="wizard-page">
          <header className="wizard-header wizard-header--shell">
            <div className="wizard-header-leading">
              <div className="wizard-brand">
                <Cat size={16} />
              </div>
              <div className="wizard-header-copy">
                <span className="wizard-title">绑定桌宠</span>
                <span className="wizard-subtitle">插网线或 Wi‑Fi 绑定。</span>
              </div>
            </div>
          </header>
          <div className="wizard-page-body">
            <DeviceSetup onComplete={handleSetupComplete} />
          </div>
        </div>
      </div>
    );
  }

  return (
    <div className="app-shell">
      <div className="app-layout">
        <aside className="app-sidebar">
          <div className="sidebar-head">
            <div className="sidebar-brand">
              <span className="sidebar-brand__mark">
                <Cat size={18} />
              </span>
              <span className="sidebar-brand__copy">
                <strong className="sidebar-brand-name">桌宠管理端</strong>
                <span>Pet Manager</span>
              </span>
            </div>
          </div>
          <nav className="sidebar-nav">
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "device" ? "is-active" : ""}`}
              onClick={() => setView(hasBinding ? "dashboard" : "setup")}
              title="设备"
            >
              <MonitorSmartphone size={16} />
              <span className="sidebar-nav-label">设备</span>
            </button>
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "gallery" ? "is-active" : ""}`}
              onClick={handleOpenGallery}
              title="形象画廊"
            >
              <ImagePlus size={16} />
              <span className="sidebar-nav-label">形象画廊</span>
            </button>
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "components" ? "is-active" : ""}`}
              onClick={handleOpenComponents}
              title="组件中心"
            >
              <Blocks size={16} />
              <span className="sidebar-nav-label">组件中心</span>
            </button>
          </nav>
          <div className="sidebar-spacer" />
          <ContextRail
            onOpenDevice={() => setView(hasBinding ? "dashboard" : "setup")}
            onOpenAppearance={handleOpenGallery}
            onOpenComponent={handleOpenComponents}
            onStartBinding={() => setView("setup")}
          />
        </aside>
        <section className="app-main">
          <main className="app-content">
            {isDashboard && binding && (
              <DeviceDashboard
                binding={binding}
                onSwitchToSetup={() => setView("setup")}
                onUnbind={handleUnbind}
                onOpenGallery={handleOpenGallery}
                onOpenDetail={handleOpenDetail}
              />
            )}
            {view === "gallery" && (
              <AppearanceGallery
                binding={binding}
                onEnterWizard={handleEnterWizard}
                onOpenDetail={handleOpenDetail}
              />
            )}
            {view === "wizard" && (
              <CustomAvatarWizard onExit={handleWizardExit} />
            )}
            {view === "detail" && detailAppearanceId && (
              <AppearanceDetail
                appearanceId={detailAppearanceId}
                onBack={handleDetailBack}
              />
            )}
            {view === "components" && (
              <ComponentCenter />
            )}
          </main>
        </section>
      </div>
    </div>
  );
}
```

Then in `App` (the default export), remove the now-unused:
- `const [toast, setToast] = useState(null)` and `const lastToastEpochRef = useRef(0)`
- The `useEffect` that calls `subscribeGenerationTask` (moved into `AppInner`)
- The `handleToastView` / `handleToastDismiss` callbacks (they can stay if you want, but the new flow handles ack inline)
- The bottom `{toast && (<ToastStack ... />)}` render

Also remove the now-unused imports `CheckCircle`, `AlertCircle`, `X` from the top of App.jsx — they were only used by the old inline ToastStack. Keep `Cat`, `ImagePlus`, `Loader2`, `MonitorSmartphone`, `Blocks` (still used).

- [ ] **Step 4: Add a sidebar spacer CSS rule (for ContextRail to sit at bottom)**

In `ref/src/styles.css`, search for `.app-sidebar` rules. If `.app-sidebar` does NOT already use `display: flex; flex-direction: column;`, add it. Then in the `/* === shell ===` section add:

```css
.sidebar-spacer {
  flex: 1;
}
```

To check existing rules:
```bash
cd /path/to/claw-pet-manager/ref
grep -n "\.app-sidebar" src/styles.css
```

If existing `.app-sidebar` is missing flex column, add it:
```css
.app-sidebar {
  display: flex;
  flex-direction: column;
  /* keep other existing rules */
}
```

- [ ] **Step 5: Run existing page tests to verify no regression**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/DeviceDashboard.test.js src/AppearanceGallery.test.js src/ComponentCenter.test.js src/DeviceSetup.test.js src/DeviceGuideModal.test.js src/AppearanceDetail.test.js src/ProductExperience.test.js
```
Expected: ALL PASS. If any fail, the failures should be related to text content in the deleted ToastStack lines (the existing static tests may grep for "toast" patterns). Fix any such test by removing the assertion that referred to the inline ToastStack code path (they no longer apply — the toast is rendered globally).

- [ ] **Step 6: Smoke test in dev**

```bash
cd /path/to/claw-pet-manager/ref
npm run dev:web
```
Open `http://localhost:4173`. Verify:
- App boots into loading then dashboard/setup
- Sidebar shows the 3 nav buttons AND the new ContextRail at the bottom
- ContextRail shows "+ 绑定设备" when not bound; shows the 3-row triad when bound
- Clicking the 3 ContextRail rows navigates to device/gallery/components
- Existing pages (device dashboard / gallery / component center) render and work exactly as before

Ctrl+C the dev server when done.

- [ ] **Step 7: Commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/App.jsx ref/src/styles.css
git commit -m "feat(shell): wire DeviceContextProvider + ToastProvider + ContextRail into App"
```

---

## Task 8: Final cross-cutting smoke + folder map update

**Files:**
- Modify: `ref/src/.folder.md` (already touched in Task 1, but now do the final update)

- [ ] **Step 1: Update `ref/src/.folder.md` Sync line**

Open `ref/src/.folder.md`. Update its top `[Output]` description (or the first paragraph) to mention the new shell folder. Add a single sentence to its Architecture block, e.g.:

> A new `shell/` subfolder hosts the cross-page shell primitives (`PageShell`, `Card`, `ContextRail`, `ToastProvider`, `DeviceContextProvider`) consumed by `App.jsx`; downstream pages migrate to the shell in Plans 2/3/4.

- [ ] **Step 2: Full shell test run**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/shell/*.test.js
```
Expected: ALL PASS (6 test files, ~31 assertions total).

- [ ] **Step 3: Full project test run**

```bash
cd /path/to/claw-pet-manager/ref
node --test src/*.test.js src/lib/*.test.js src/shell/*.test.js
```
Expected: ALL PASS. If any pre-existing tests fail because they referenced text moved out of App.jsx, fix those tests by deleting/updating the assertions that no longer apply.

- [ ] **Step 4: Final commit**

```bash
cd /path/to/claw-pet-manager
git add ref/src/.folder.md
git commit -m "docs(shell): update src/.folder.md with shell subfolder reference"
```

---

## Definition of Done for Plan 1

- [ ] `ref/src/shell/` contains 5 components + 5 tests + `.folder.md`
- [ ] `App.jsx` is wrapped in `ToastProvider` + `DeviceContextProvider`, the inline ToastStack is gone, sidebar contains ContextRail
- [ ] `styles.css` has a `/* === shell ===` section with PageShell/Card/ContextRail rules (toast rules adjusted as needed)
- [ ] All shell tests pass via `node --test src/shell/*.test.js`
- [ ] All existing tests pass via `node --test src/*.test.js src/lib/*.test.js`
- [ ] App boots in `npm run dev:web`, ContextRail visible in sidebar, no visual regression on the 3 existing pages
- [ ] **Shell API is frozen** — Plans 2/3/4 may NOT modify any file under `ref/src/shell/` or change the documented props of the 5 components

---

## Self-Review (run by the writer, not by an agent)

**Spec coverage:**
- Spec § Shared Shell PageShell → Task 2 ✓
- Spec § Shared Shell Card / Card.Collapsible → Task 3 ✓
- Spec § Shared Shell ContextRail → Task 6 ✓
- Spec § Shared Shell ToastStack + useToast → Task 4 ✓
- Spec § Shared Shell DeviceContextProvider → Task 5 ✓
- Spec § "Card 不带 tone" → encoded as a negative-assertion test in Task 3 ✓
- Spec § "PageShell help 不强制" → `help && (...)` guard tested in Task 2 ✓
- Spec § "未绑定时 ContextRail 收成 + 绑定设备" → Task 6 test + impl ✓
- Spec § "DeviceContextProvider 接管 3 页轮询" → Task 5 USB/online/appearance/agent effects ✓
- Spec § "保留 message-banner" → out of scope for Plan 1 (each page keeps its own banners until Plan 2/3/4) ✓
- Spec § "shell API frozen post Plan 1" → Definition of Done note ✓

**Placeholder scan:** None. All steps contain concrete code, exact paths, exact commands, exact commit messages.

**Type consistency:** Cross-checked. `useToast()` returns `{ push, dismiss, items }` consistently across Task 4 implementation and Task 7 consumer. `useDeviceContext()` returns the shape asserted in Task 5 test and consumed in Task 6 (`binding`, `usb`, `deviceOnline`, `currentDisplay`, `currentComponent`) and Task 7 (same fields). `applyDesktopPet(agentId, appearance, options)` signature matches across Task 5 definition and the spec.

**Known fragile area:** Task 7's existing-tests regression check is the highest-risk step. The existing static tests for `DeviceDashboard.jsx` / `AppearanceGallery.jsx` / `ComponentCenter.jsx` read those source files directly; they should NOT be affected by App.jsx changes. The `ProductExperience.test.js` file might assert on App.jsx content — that's where any regression would surface. If it fires on the missing inline ToastStack, fix by updating the assertion to point at `shell/ToastStack.jsx` instead.

---
