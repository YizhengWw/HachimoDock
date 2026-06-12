/**
 * [Input] Bridge device availability map and local saved device binding.
 * [Output] Helpers that resolve the currently online board for USB-or-wireless device actions,
 *          including fallback from stale saved board ids to MQTT devices targeting the same desktop.
 * [Pos] shared library node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

function normalizeId(value) {
  return String(value || "").trim();
}

function entryTargetsDesktop(entry, desktopDeviceId) {
  const target = normalizeId(entry?.targetDeviceId || entry?.desktopDeviceId);
  return Boolean(target && desktopDeviceId && target === desktopDeviceId);
}

export function resolveOnlineBoardDeviceId(devices, binding) {
  const map = devices && typeof devices === "object" ? devices : {};
  const boundBoardId = normalizeId(binding?.boardDeviceId);
  const desktopDeviceId = normalizeId(binding?.desktopDeviceId);

  if (boundBoardId && map[boundBoardId]?.online === true) {
    return boundBoardId;
  }

  for (const [boardDeviceId, entry] of Object.entries(map)) {
    if (entry?.online === true && entryTargetsDesktop(entry, desktopDeviceId)) {
      return normalizeId(entry?.boardDeviceId) || boardDeviceId;
    }
  }

  const onlineBoards = Object.entries(map)
    .filter(([, entry]) => entry?.online === true)
    .map(([boardDeviceId, entry]) => normalizeId(entry?.boardDeviceId) || boardDeviceId)
    .filter(Boolean);
  if (onlineBoards.length === 1) {
    return onlineBoards[0];
  }

  return "";
}

export function isBoundDeviceOnline(devices, binding) {
  return Boolean(resolveOnlineBoardDeviceId(devices, binding));
}
