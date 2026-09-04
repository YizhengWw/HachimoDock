/**
 * [Input] onboarding-state.js public helpers.
 * [Output] Runtime coverage for independent first-visit state, stable legacy
 *          device storage, and failure-safe storage access.
 * [Pos] test node in pc/src/lib
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  ONBOARDING_PAGE_IDS,
  clearOnboardingSeen,
  hasSeenOnboarding,
  markOnboardingSeen,
  onboardingStorageKey,
  shouldAutoOpenOnboarding,
} from "./onboarding-state.js";

function memoryStorage() {
  const values = new Map();
  return {
    getItem(key) {
      return values.has(key) ? values.get(key) : null;
    },
    setItem(key, value) {
      values.set(key, value);
    },
    removeItem(key) {
      values.delete(key);
    },
  };
}

test("onboarding pages persist first visits independently", () => {
  const storage = memoryStorage();

  assert.equal(
    shouldAutoOpenOnboarding(ONBOARDING_PAGE_IDS.APPEARANCE_GALLERY, storage),
    true,
  );
  assert.equal(
    shouldAutoOpenOnboarding(ONBOARDING_PAGE_IDS.COMPONENT_CENTER, storage),
    true,
  );

  assert.equal(
    markOnboardingSeen(ONBOARDING_PAGE_IDS.APPEARANCE_GALLERY, storage),
    true,
  );
  assert.equal(hasSeenOnboarding(ONBOARDING_PAGE_IDS.APPEARANCE_GALLERY, storage), true);
  assert.equal(hasSeenOnboarding(ONBOARDING_PAGE_IDS.COMPONENT_CENTER, storage), false);

  assert.equal(
    clearOnboardingSeen(ONBOARDING_PAGE_IDS.APPEARANCE_GALLERY, storage),
    true,
  );
  assert.equal(hasSeenOnboarding(ONBOARDING_PAGE_IDS.APPEARANCE_GALLERY, storage), false);
});

test("device onboarding keeps its existing storage key", () => {
  assert.equal(
    onboardingStorageKey(ONBOARDING_PAGE_IDS.DEVICE),
    "pet-manager.device-guide-seen",
  );
});

test("onboarding state fails open when storage is unavailable", () => {
  const brokenStorage = {
    getItem() {
      throw new Error("blocked");
    },
    setItem() {
      throw new Error("blocked");
    },
    removeItem() {
      throw new Error("blocked");
    },
  };

  assert.equal(hasSeenOnboarding("example", brokenStorage), false);
  assert.equal(shouldAutoOpenOnboarding("example", brokenStorage), true);
  assert.equal(markOnboardingSeen("example", brokenStorage), false);
  assert.equal(clearOnboardingSeen("example", brokenStorage), false);
});
