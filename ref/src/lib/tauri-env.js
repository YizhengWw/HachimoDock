/**
 * [Input] The ambient browser/Tauri window.
 * [Output] `hasTauriRuntime()` — single source of truth for "are we running
 *          inside the Tauri shell (vs a plain web preview)". Previously copied
 *          verbatim into 5 files; centralized here.
 * [Pos] lib helper for ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export function hasTauriRuntime() {
  return typeof window !== "undefined" && Boolean(window.__TAURI_INTERNALS__);
}
