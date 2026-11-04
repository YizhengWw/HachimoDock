/**
 * [Input] Device and bundled firmware version strings such as `0.7.34-p4` or `0.7.103-p4-r144`.
 * [Output] Numeric comparison (the `-rNNN` build revision breaks ties between equal
 *          numeric cores) plus update/latest/unknown sidebar disposition.
 * [Pos] Pure firmware-version helper in pc/src/lib.
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

export function parseFirmwareVersion(value) {
  const normalized = String(value || "").trim().replace(/^v/i, "");
  const core = normalized.split("-", 1)[0];
  if (!/^\d+(?:\.\d+){0,3}$/.test(core)) return null;
  return core.split(".").map(Number);
}

// Build revision `-rNNN` of a P4 label (`0.7.103-p4-r144`, `0.7.103-p4-r142-framework`).
// Labels without one (`0.7.34-p4`) count as revision 0 so a revisioned build of the
// same numeric core is newer than an unrevisioned one.
export function parseFirmwareRevision(value) {
  const match = /-r(\d{1,6})(?:-|$)/.exec(String(value || "").trim());
  return match ? Number(match[1]) : 0;
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
  const revisionDelta = parseFirmwareRevision(left) - parseFirmwareRevision(right);
  if (revisionDelta < 0) return -1;
  if (revisionDelta > 0) return 1;
  return 0;
}

export function firmwareUpdateDisposition(currentVersion, bundledVersion) {
  const comparison = compareFirmwareVersions(currentVersion, bundledVersion);
  if (comparison === null) return "unknown";
  return comparison < 0 ? "update" : "latest";
}
