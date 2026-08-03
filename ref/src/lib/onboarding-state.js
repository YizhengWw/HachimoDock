/**
 * [Input] Page onboarding id plus optional Web Storage-compatible persistence.
 * [Output] Stable per-page storage keys and failure-safe seen/read/write helpers
 *          shared by device, appearance-gallery, and component-center guidance.
 * [Pos] onboarding state helper in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export const ONBOARDING_PAGE_IDS = Object.freeze({
  DEVICE: "device",
  APPEARANCE_GALLERY: "appearance-gallery",
  COMPONENT_CENTER: "component-center",
});

export const ONBOARDING_STORAGE_KEYS = Object.freeze({
  [ONBOARDING_PAGE_IDS.DEVICE]: "pet-manager.device-guide-seen",
  [ONBOARDING_PAGE_IDS.APPEARANCE_GALLERY]:
    "pet-manager.onboarding.appearance-gallery.v2",
  [ONBOARDING_PAGE_IDS.COMPONENT_CENTER]:
    "pet-manager.onboarding.component-center.v2",
});

function resolveStorage(storage) {
  if (storage) return storage;
  if (typeof window === "undefined") return null;
  try {
    return window.localStorage || null;
  } catch {
    return null;
  }
}

export function onboardingStorageKey(pageId) {
  return ONBOARDING_STORAGE_KEYS[pageId] || `pet-manager.onboarding.${pageId}.v1`;
}

export function hasSeenOnboarding(pageId, storage) {
  const target = resolveStorage(storage);
  if (!target) return false;
  try {
    return target.getItem(onboardingStorageKey(pageId)) === "1";
  } catch {
    return false;
  }
}

export function shouldAutoOpenOnboarding(pageId, storage) {
  return !hasSeenOnboarding(pageId, storage);
}

export function markOnboardingSeen(pageId, storage) {
  const target = resolveStorage(storage);
  if (!target) return false;
  try {
    target.setItem(onboardingStorageKey(pageId), "1");
    return true;
  } catch {
    return false;
  }
}

export function clearOnboardingSeen(pageId, storage) {
  const target = resolveStorage(storage);
  if (!target) return false;
  try {
    target.removeItem(onboardingStorageKey(pageId));
    return true;
  } catch {
    return false;
  }
}
