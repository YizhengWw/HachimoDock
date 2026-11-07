/**
 * [Input] Browser localStorage bubble-opacity preference.
 * [Output] A bounded P4 bubble opacity that keeps text readable.
 * [Pos] shared display preference helper in pc/src/lib
 */

export const BUBBLE_OPACITY_STORAGE_KEY = "hachimodock.bubble-opacity.v1";
export const BUBBLE_OPACITY_MIN = 20;
export const BUBBLE_OPACITY_MAX = 100;
export const BUBBLE_OPACITY_DEFAULT = 100;

export function normalizeBubbleOpacity(value) {
  const number = Math.round(Number(value));
  if (!Number.isFinite(number)) return BUBBLE_OPACITY_DEFAULT;
  return Math.min(BUBBLE_OPACITY_MAX, Math.max(BUBBLE_OPACITY_MIN, number));
}

export function loadBubbleOpacity() {
  try {
    return normalizeBubbleOpacity(window.localStorage.getItem(BUBBLE_OPACITY_STORAGE_KEY));
  } catch {
    return BUBBLE_OPACITY_DEFAULT;
  }
}

export function saveBubbleOpacity(value) {
  const next = normalizeBubbleOpacity(value);
  window.localStorage.setItem(BUBBLE_OPACITY_STORAGE_KEY, String(next));
  return next;
}
