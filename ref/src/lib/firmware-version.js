/**
 * [Input] Device and bundled firmware version strings such as `0.7.32-p4`.
 * [Output] Numeric comparison plus update/latest/unknown sidebar disposition.
 * [Pos] Pure firmware-version helper in ref/src/lib.
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export function parseFirmwareVersion(value) {
  const normalized = String(value || "").trim().replace(/^v/i, "");
  const core = normalized.split("-", 1)[0];
  if (!/^\d+(?:\.\d+){0,3}$/.test(core)) return null;
  return core.split(".").map(Number);
}

export function compareFirmwareVersions(left, right) {
  const leftParts = parseFirmwareVersion(left);
  const rightParts = parseFirmwareVersion(right);
  if (!leftParts || !rightParts) return null;
  const width = Math.max(leftParts.length, rightParts.length);
  for (let index = 0; index < width; index += 1) {
    const delta = (leftParts[index] || 0) - (rightParts[index] || 0);
    if (delta < 0) return -1;
    if (delta > 0) return 1;
  }
  return 0;
}

export function firmwareUpdateDisposition(currentVersion, bundledVersion) {
  const comparison = compareFirmwareVersions(currentVersion, bundledVersion);
  if (comparison === null) return "unknown";
  return comparison < 0 ? "update" : "latest";
}
