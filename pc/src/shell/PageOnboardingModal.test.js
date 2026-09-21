/**
 * [Input] PageOnboardingModal.jsx source and shared stylesheet.
 * [Output] Static coverage for first-visit modal persistence, optional reminders,
 *          direct actions, accessible close behavior, and responsive layout.
 * [Pos] test node in pc/src/shell
 * [Sync] If this file changes, update `pc/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "PageOnboardingModal.jsx"), "utf8");
const styles = readFileSync(join(here, "..", "styles.css"), "utf8");

test("page onboarding auto-opens and persists the reminder choice on close", () => {
  assert.match(source, /shouldAutoOpenOnboarding\(pageId\)/);
  assert.match(source, /const show = useCallback\(\(\) => setOpen\(true\)/);
  assert.match(source, /if \(dontShowAgain\) markOnboardingSeen\(pageId\)/);
  assert.match(source, /else clearOnboardingSeen\(pageId\)/);
  assert.match(source, /const \[dontShowAgain, setDontShowAgain\] = useState\(true\)/);
  assert.match(source, /label="下次不再自动弹出"/);
});

test("page onboarding is an accessible, dismissible modal with direct actions", () => {
  assert.match(source, /className="modal-card page-onboarding-modal"/);
  assert.match(source, /role="dialog"/);
  assert.match(source, /aria-modal="true"/);
  assert.match(source, /aria-labelledby=\{titleId\}/);
  assert.match(source, /event\.key === "Escape"/);
  assert.match(source, /className="page-onboarding-modal__steps"/);
  assert.match(source, /action\.onClick\?\.\(\)/);
  assert.match(source, />\s*知道了\s*</);
  assert.match(styles, /\.page-onboarding-modal\s*\{/);
  assert.match(styles, /\.page-onboarding-modal__steps\s*\{[\s\S]*repeat\(3/);
  assert.match(styles, /@media \(max-width: 640px\)[\s\S]*\.page-onboarding-modal__steps/);
});
