/**
 * [Input] Browser localStorage keep-awake preference.
 * [Output] Whether HachimoDock should request a system-sleep-blocking assertion.
 * [Pos] shared macOS power preference helper in pc/src/lib
 */

export const KEEP_AWAKE_STORAGE_KEY = "hachimodock.keep-awake.v1";
export const KEEP_AWAKE_DEFAULT = false;

export function normalizeKeepAwake(value) {
  return value === true || value === "true";
}

export function loadKeepAwake() {
  try {
    return normalizeKeepAwake(window.localStorage.getItem(KEEP_AWAKE_STORAGE_KEY));
  } catch {
    return KEEP_AWAKE_DEFAULT;
  }
}

export function saveKeepAwake(value) {
  const next = normalizeKeepAwake(value);
  window.localStorage.setItem(KEEP_AWAKE_STORAGE_KEY, String(next));
  return next;
}
