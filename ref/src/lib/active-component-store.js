/**
 * [Input] Browser storage plus an explicit USB boardDeviceId or SSH host target.
 * [Output] Versioned, per-device active-component records with exact formal-library
 *          identity; legacy global and draft records remain readable compatibility data.
 * [Pos] shared component lifecycle store in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export const ACTIVE_COMPONENT_STORAGE_KEY = "pet-manager:active-component";
export const ACTIVE_COMPONENT_STORE_VERSION = 2;
export const COMPONENT_SSH_HOST_STORAGE_KEY = "petManager.sshHost";

function storageOrNull(storage) {
  if (storage) return storage;
  if (typeof window === "undefined") return null;
  return window.localStorage || null;
}

export function normalizeActiveComponentTarget(target) {
  const transport = String(target?.transport || "").trim().toLowerCase();
  if (transport === "usb") {
    const boardDeviceId = String(target?.boardDeviceId || "").trim();
    return boardDeviceId ? { transport, boardDeviceId } : null;
  }
  if (transport === "ssh") {
    const sshHost = String(target?.sshHost || "").trim();
    return sshHost ? { transport, sshHost } : null;
  }
  return null;
}

export function activeComponentTargetKey(target) {
  const normalized = normalizeActiveComponentTarget(target);
  if (!normalized) return "";
  return normalized.transport === "usb"
    ? `usb:${normalized.boardDeviceId}`
    : `ssh:${normalized.sshHost}`;
}

export function readConfiguredComponentSshHost(storage) {
  try {
    return String(storageOrNull(storage)?.getItem(COMPONENT_SSH_HOST_STORAGE_KEY) || "").trim();
  } catch {
    return "";
  }
}

export function loadActiveComponentStore(storage) {
  try {
    const raw = storageOrNull(storage)?.getItem(ACTIVE_COMPONENT_STORAGE_KEY);
    if (!raw) return { version: ACTIVE_COMPONENT_STORE_VERSION, activeByTarget: {} };
    const parsed = JSON.parse(raw);
    if (
      parsed?.version === ACTIVE_COMPONENT_STORE_VERSION
      && parsed.activeByTarget
      && typeof parsed.activeByTarget === "object"
      && !Array.isArray(parsed.activeByTarget)
    ) {
      return {
        version: ACTIVE_COMPONENT_STORE_VERSION,
        activeByTarget: parsed.activeByTarget,
        lastTargetKey: typeof parsed.lastTargetKey === "string" ? parsed.lastTargetKey : "",
      };
    }
    if (parsed && typeof parsed === "object" && typeof parsed.id === "string" && parsed.id) {
      return {
        version: 1,
        activeByTarget: {},
        legacy: { id: parsed.id, name: parsed.name || parsed.id },
      };
    }
  } catch {
    // Corrupt browser state is treated as empty.
  }
  return { version: ACTIVE_COMPONENT_STORE_VERSION, activeByTarget: {} };
}

export function readActiveComponentForTarget(target, storage) {
  const store = loadActiveComponentStore(storage);
  const targetKey = activeComponentTargetKey(target);
  if (store.version === ACTIVE_COMPONENT_STORE_VERSION) {
    const record = targetKey ? store.activeByTarget[targetKey] : null;
    if (record?.id) {
      return { ...record, targetVerified: true, targetKey };
    }
    if (!targetKey && store.lastTargetKey) {
      const last = store.activeByTarget[store.lastTargetKey];
      if (last?.id) {
        return {
          ...last,
          targetVerified: false,
          targetKey: store.lastTargetKey,
        };
      }
    }
    return null;
  }
  return store.legacy
    ? { ...store.legacy, target: null, targetVerified: false, targetKey: "" }
    : null;
}

export function writeActiveComponentForTarget(component, target, storage) {
  const normalizedTarget = normalizeActiveComponentTarget(target);
  const targetKey = activeComponentTargetKey(normalizedTarget);
  if (!component?.id || !targetKey) {
    throw new Error("启用组件必须绑定明确的 USB boardDeviceId 或 SSH 主机");
  }
  const current = loadActiveComponentStore(storage);
  const activeByTarget = current.version === ACTIVE_COMPONENT_STORE_VERSION
    ? { ...current.activeByTarget }
    : {};
  let source = { type: "builtin" };
  if (component.isLocal || component.libraryPath) {
    source = {
      type: "library",
      path: String(component.libraryPath || component.path || ""),
    };
  } else if (component.isDraft || component.draftPath) {
    source = {
      type: "draft",
      path: String(component.draftPath || component.path || ""),
    };
  }
  const record = {
    id: component.id,
    name: component.name || component.id,
    target: normalizedTarget,
    source,
  };
  activeByTarget[targetKey] = record;
  storageOrNull(storage)?.setItem(
    ACTIVE_COMPONENT_STORAGE_KEY,
    JSON.stringify({
      version: ACTIVE_COMPONENT_STORE_VERSION,
      activeByTarget,
      lastTargetKey: targetKey,
    }),
  );
  return { ...record, targetVerified: true, targetKey };
}

export function removeActiveComponentForTarget(target, storage) {
  const targetKey = activeComponentTargetKey(target);
  if (!targetKey) return false;
  const current = loadActiveComponentStore(storage);
  if (
    current.version !== ACTIVE_COMPONENT_STORE_VERSION
    || !current.activeByTarget[targetKey]
  ) {
    return false;
  }
  const activeByTarget = { ...current.activeByTarget };
  delete activeByTarget[targetKey];
  const remainingKeys = Object.keys(activeByTarget);
  const lastTargetKey = current.lastTargetKey === targetKey
    ? (remainingKeys[0] || "")
    : current.lastTargetKey;
  storageOrNull(storage)?.setItem(
    ACTIVE_COMPONENT_STORAGE_KEY,
    JSON.stringify({
      version: ACTIVE_COMPONENT_STORE_VERSION,
      activeByTarget,
      lastTargetKey,
    }),
  );
  return true;
}
