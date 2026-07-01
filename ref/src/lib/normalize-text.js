/**
 * [Input] An arbitrary value plus an optional fallback.
 * [Output] `normalizeText(value, fallback)` — trims the value to a string, or the
 *          trimmed fallback when empty. Previously copied verbatim into 3 contract/
 *          runtime modules; centralized here. (Note: DeviceDashboard has a distinct
 *          single-arg normalizeText with different semantics and is unrelated.)
 * [Pos] lib helper for ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export function normalizeText(value, fallback = "") {
  const normalized = String(value || "").trim();
  return normalized || String(fallback || "").trim();
}
