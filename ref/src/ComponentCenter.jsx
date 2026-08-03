/**
 * [Input] Consume component-center package fixtures from `ref/src/fixtures.js`[Pos].
 * [Output] Component manager in the shared Pet Manager visual system: one type-filtered
 *          game/tool library whose cards reflect live board truth and perform immediate
 *          per-component sync/removal. The manager chrome stays product-native while every widget
 *          preview and generated package uses the bounded pixel visual contract. Formal local
 *          buttons.json is preserved per component, editable across screen/SW1-SW3/
 *          encoder inputs, conflict-checked, persisted by component/action, and stored
 *          inside the installed P4 component without overwriting device navigation.
 *          Live inventory, not localStorage, is authoritative for what is on the board. The
 *          latest result is retained only in module memory for the current App session, so
 *          returning to Component Center does not re-query the board while a full App restart does.
 *          Formal local deletion is a device-first transaction when the package is installed,
 *          so a failed board ACK never destroys the only local source. Builtins keep their
 *          product-defined order; formal local packages follow newest-first with stable ties.
 *          A first-visit quick-start modal explains browse/create and direct card sync, and
 *          remains reopenable from the page help action. Component generation stays
 *          in the user's current Agent conversation: petui validates and atomically publishes
 *          to the formal local library, which this page watches and refreshes without launching CLIs.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  Clipboard,
  Gamepad2,
  LayoutGrid,
  PackageCheck,
  Plus,
  RefreshCw,
  Sparkles,
  Trash2,
  Unplug,
  Wrench,
  X,
} from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import { open as openDialog } from "@tauri-apps/plugin-dialog";
import { watch } from "@tauri-apps/plugin-fs";
import { BUILTIN_COMPONENT_CENTER } from "./fixtures";
import {
  activeComponentTargetKey,
  readConfiguredComponentSshHost,
  removeActiveComponentForTarget,
  writeActiveComponentForTarget,
} from "./lib/active-component-store.js";
import PageShell from "./shell/PageShell.jsx";
import PageOnboardingModal, {
  usePageOnboarding,
} from "./shell/PageOnboardingModal.jsx";
import { useToast } from "./shell/ToastStack.jsx";
import { useDeviceContext } from "./shell/DeviceContext.jsx";
import { ONBOARDING_PAGE_IDS } from "./lib/onboarding-state.js";
import CandidateCard, { resolveComponentKind } from "./component-center/CandidateCard.jsx";
import ComponentPreviewModal from "./component-center/ComponentPreviewModal.jsx";
import { isRoutedWidgetBinding } from "./component-center/binding-labels.js";
import { sortComponentsByCreatedAt } from "./component-center/library-order.js";
import {
  COMPONENT_CONTROL_OPTIONS,
  componentInputEventSlots,
  defaultControlLabelForBinding,
  optionForControlLabel,
} from "./component-center/button-config.js";

const COMPONENT_BUTTON_OVERRIDES_STORAGE_KEY = "pet-manager:component-button-overrides:v1";
const EMPTY_DEVICE_INVENTORY = Object.freeze({
  freshness: "idle",
  runtime: "",
  activeWidgetId: "",
  supportsMultiple: true,
  maxInstalled: null,
  items: [],
  warnings: [],
});
const SESSION_DEVICE_INVENTORY_CACHE = new Map();
const SESSION_DEVICE_INVENTORY_REQUESTS = new Map();

const CONTROL_HELP = Object.fromEntries(
  COMPONENT_CONTROL_OPTIONS.map((option) => [option.label, option.help]),
);

function gameInstallBlockedReason(component, usb) {
  const requiredRuntime = String(component?.runtimeEngine || "");
  const requiredScene = String(component?.sceneEngine || "");
  const preset = String(component?.gamePreset || component?.gameType || "");
  if (!requiredRuntime && !requiredScene && !preset) return "";
  const runtime = String(usb?.capabilities?.widgetRuntime || "");
  const scene = String(usb?.capabilities?.widgetScene || "");
  const presets = Array.isArray(usb?.capabilities?.widgetGamePresets)
    ? usb.capabilities.widgetGamePresets
    : Array.isArray(usb?.capabilities?.widgetGames)
      ? usb.capabilities.widgetGames
    : [];
  if (requiredRuntime && runtime !== requiredRuntime) {
    return `这个组件需要 P4 通用运行时（${requiredRuntime}），请先升级设备固件。`;
  }
  if (requiredScene && scene !== requiredScene) {
    return `这个组件需要 P4 场景能力（${requiredScene}），请先升级设备固件。`;
  }
  if (preset && !presets.includes(preset)) {
    return `这个旧版游戏预设需要设备能力（${preset}），请先升级设备固件。`;
  }
  if (preset && !["p4-bounded-v2", "p4-bounded-runtime-v3"].includes(runtime)) {
    return `这个旧版游戏预设需要 P4 组件运行时，请先升级设备固件。`;
  }
  return "";
}

function localInstallBlockedReason(component) {
  if (!component?.isLocal || component.valid !== false) return "";
  const errors = Array.isArray(component.validationErrors)
    ? component.validationErrors.filter(Boolean)
    : [];
  return `正式本地组件校验失败：${errors.join("；") || "组件包结构或运行时声明无效"}`;
}

function deviceTouchReady(usb) {
  const reportedReady = usb?.capabilities?.touchInput?.ready;
  if (typeof reportedReady === "boolean") return reportedReady;
  return usb?.capabilities?.touch === true;
}

function loadComponentButtonOverrides() {
  if (typeof window === "undefined" || !window.localStorage) return {};
  try {
    const parsed = JSON.parse(
      window.localStorage.getItem(COMPONENT_BUTTON_OVERRIDES_STORAGE_KEY) || "{}",
    );
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) return {};
    return Object.fromEntries(
      Object.entries(parsed).filter(
        ([key, value]) => (
          typeof key === "string"
          && typeof value === "string"
          && Boolean(optionForControlLabel(value))
        ),
      ),
    );
  } catch {
    return {};
  }
}

function normalizeDashboardProgress(progress) {
  if (!progress) return null;
  if (typeof progress === "object") {
    const value = Number(progress.value);
    if (!Number.isFinite(value)) return null;
    return {
      value: Math.max(0, Math.min(100, value)),
      label: typeof progress.label === "string" ? progress.label : "",
    };
  }
  if (typeof progress === "string") {
    const [rawValue, ...labelParts] = progress.split(":");
    const value = Number(rawValue);
    if (!Number.isFinite(value)) return null;
    return {
      value: Math.max(0, Math.min(100, value)),
      label: labelParts.join(":"),
    };
  }
  return null;
}

function buildLibraryGoal(component) {
  const description = typeof component.description === "string" ? component.description.trim() : "";
  return description || "自定义组件 · 可预览后添加到负一屏。";
}

function normalizeLocalPath(value) {
  return String(value || "").replaceAll("\\", "/");
}

function componentSourceKey(component) {
  const libraryPath = component?.libraryPath || (component?.isLocal ? component?.path : "");
  if (component?.isLocal || libraryPath) {
    return `library:${normalizeLocalPath(libraryPath)}`;
  }
  if (component?.isDraft || component?.draftPath) {
    return `draft:${normalizeLocalPath(component.draftPath || component.path)}`;
  }
  return component?.id ? `builtin:${component.id}` : "";
}

function activeRecordSourceKey(record) {
  if (record?.source?.type === "library" && record.source.path) {
    return `library:${normalizeLocalPath(record.source.path)}`;
  }
  if (record?.source?.type === "draft" && record.source.path) {
    return `draft:${normalizeLocalPath(record.source.path)}`;
  }
  return record?.id ? `builtin:${record.id}` : "";
}

function bindingKey(component, action) {
  return `${componentSourceKey(component)}:${action}`;
}

function componentTargetLabel(target) {
  if (target?.transport === "usb") {
    return `USB · ${target.boardDeviceId || "未识别设备"}`;
  }
  if (target?.transport === "ssh") {
    return `SSH · ${target.sshHost || "未设置主机"}`;
  }
  return "未确认设备";
}

function inventoryWarningMessage(value) {
  const warning = String(value || "").trim();
  if (!warning) return "";
  if (warning === "inventory-limit-reached") {
    return "设备组件仓库已达到容量上限，请先移除不再使用的组件。";
  }
  if (warning === "active-package-missing") {
    return "设备记录的当前组件包已经缺失，请重新启用一个组件。";
  }
  if (warning === "manifest-invalid") {
    return "板端有组件包的清单损坏；可移除后从组件库重新添加。";
  }
  if (/^[a-z0-9_-]+(?::[a-z0-9._-]+)?$/i.test(warning)) {
    return `设备报告组件仓库异常（${warning}），请刷新后重试。`;
  }
  return warning;
}

function normalizeDeviceInventory(payload) {
  const items = Array.isArray(payload?.items)
    ? payload.items
      .filter((item) => item && typeof item.id === "string" && item.id)
      .map((item, index) => ({
        id: item.id,
        name: typeof item.name === "string" ? item.name : "",
        kind: (item.kind || item.gameType)
          ? resolveComponentKind(item.kind, item.gameType)
          : "",
        version: typeof item.version === "string" ? item.version : "",
        active: item.active === true || item.id === payload?.activeWidgetId,
        manifestState: item.manifestState || "missing",
        removable: item.removable !== false,
        slot: index + 1,
      }))
    : [];
  return {
    freshness: ["live", "stale"].includes(payload?.freshness)
      ? payload.freshness
      : "unsupported",
    runtime: typeof payload?.runtime === "string" ? payload.runtime : "",
    transport: typeof payload?.transport === "string" ? payload.transport : "",
    activeWidgetId: typeof payload?.activeWidgetId === "string"
      ? payload.activeWidgetId
      : items.find((item) => item.active)?.id || "",
    supportsMultiple: payload?.supportsMultiple !== false,
    maxInstalled: Number.isFinite(payload?.maxInstalled) ? payload.maxInstalled : null,
    items,
    warnings: Array.isArray(payload?.warnings)
      ? payload.warnings.map(inventoryWarningMessage).filter(Boolean)
      : [],
  };
}

function inventoryFailure(error, target) {
  const detail = typeof error === "string" ? error : String(error || "");
  const unsupported = target?.transport === "usb" && (
    /unsupported|unknown topic|固件|widgetInventory|capabilit/i.test(detail)
  );
  const unsupportedReason = detail
    ? `${detail} App 因此不会用本机记录代替板端真实清单。`
    : "当前板端固件未声明组件清单能力 widgetInventory；请升级固件并重新连接。";
  return {
    ...EMPTY_DEVICE_INVENTORY,
    freshness: unsupported ? "unsupported" : "error",
    transport: target?.transport || "",
    supportsMultiple: target?.transport !== "usb",
    maxInstalled: target?.transport === "usb" ? 1 : null,
    warnings: [
      unsupported
        ? unsupportedReason
        : `读取设备组件失败：${detail || "设备没有响应"}`,
    ],
  };
}

function readSessionDeviceInventory(target) {
  const targetKey = activeComponentTargetKey(target);
  return targetKey ? SESSION_DEVICE_INVENTORY_CACHE.get(targetKey) || null : null;
}

function writeSessionDeviceInventory(target, inventory) {
  const targetKey = activeComponentTargetKey(target);
  if (targetKey && inventory) {
    SESSION_DEVICE_INVENTORY_CACHE.set(targetKey, inventory);
  }
  return inventory;
}

function requestSessionDeviceInventory(target) {
  const targetKey = activeComponentTargetKey(target);
  if (!targetKey) return Promise.resolve(EMPTY_DEVICE_INVENTORY);
  const existingRequest = SESSION_DEVICE_INVENTORY_REQUESTS.get(targetKey);
  if (existingRequest) return existingRequest;

  const request = invoke("list_device_widgets", {
    input: {
      transport: target.transport,
      boardDeviceId: target.boardDeviceId || "",
      sshHost: target.sshHost || "",
    },
  }).then((result) => writeSessionDeviceInventory(
    target,
    normalizeDeviceInventory(result),
  )).catch((error) => {
    console.warn("[ComponentCenter] list_device_widgets failed", error);
    return writeSessionDeviceInventory(target, inventoryFailure(error, target));
  }).finally(() => {
    if (SESSION_DEVICE_INVENTORY_REQUESTS.get(targetKey) === request) {
      SESSION_DEVICE_INVENTORY_REQUESTS.delete(targetKey);
    }
  });
  SESSION_DEVICE_INVENTORY_REQUESTS.set(targetKey, request);
  return request;
}

function buildUnknownInventoryComponent(item) {
  const name = item?.name || item?.id || "未知组件";
  return {
    id: item?.id || "unknown-widget",
    name,
    isDeviceOnly: true,
    kind: item?.kind || "tool",
    goal: "这个组件来自设备，当前本机组件库没有对应安装源。",
    defaultBindings: [],
    dashboard: {
      title: name,
      eyebrow: "BOARD PACKAGE",
      headline: item?.active ? "运行中" : "已安装",
      metricLabel: "版本",
      metricValue: item?.version || "READY",
      note: "DEVICE ONLY",
      footer: "可从设备安全移除",
      visualStyle: "pixel",
      visualPalette: item?.active ? "mint" : "arcade",
      visualLayout: "scoreboard",
      visualSprite: item?.active ? "star" : "bolt",
    },
  };
}

function componentActionErrorMessage(error) {
  const detail = typeof error === "string" ? error : String(error || "");
  if (/phase=unknown|widgetDelete|unsupported|固件/i.test(detail)) {
    return "当前设备固件不支持可靠的组件删除。请升级固件、重启 Pet Manager 并重新连接；本机状态尚未清除。";
  }
  if (/未收到板端组件 OTA 确认|widget.*ack.*timed out|timed out|timeout/i.test(detail)) {
    return "设备没有确认删除，可能是连接中断或旧固件导致。请确认设备在线后重试；若仍出现，请升级固件并重启 Pet Manager。本机状态尚未清除。";
  }
  return detail || "设备操作失败";
}

function pathContainsComponentId(value, componentId) {
  const id = String(componentId || "").trim();
  if (!id) return false;
  return normalizeLocalPath(value)
    .split("/")
    .some((segment) => segment === id || segment === `${id}.clawpkg` || segment === `${id}.zip`);
}

function matchesLibraryPath(component, clawpkgPath) {
  if (!component || !clawpkgPath) return false;
  return normalizeLocalPath(component.path) === normalizeLocalPath(clawpkgPath)
    || pathContainsComponentId(clawpkgPath, component.id);
}

export default function ComponentCenter() {
  const { push } = useToast();
  const onboarding = usePageOnboarding(ONBOARDING_PAGE_IDS.COMPONENT_CENTER);
  const {
    usb,
    currentComponent: deviceCurrentComponent,
  } = useDeviceContext();
  const configuredSshHost = readConfiguredComponentSshHost();
  const liveInventoryTarget = useMemo(() => {
    const boardDeviceId = String(usb.boardDeviceId || "").trim();
    if (usb.connected && boardDeviceId) {
      return { transport: "usb", boardDeviceId };
    }
    return configuredSshHost
      ? { transport: "ssh", sshHost: configuredSshHost }
      : null;
  }, [configuredSshHost, usb.boardDeviceId, usb.connected]);
  const deviceConnected = Boolean(liveInventoryTarget);

  const [createDrawerOpen, setCreateDrawerOpen] = useState(false);
  const [previewComponent, setPreviewComponent] = useState(null);
  const [activeComponentRecord, setActiveComponentRecord] = useState(deviceCurrentComponent);
  const [bindingOverrides, setBindingOverrides] = useState(loadComponentButtonOverrides);
  const [skillInstalling, setSkillInstalling] = useState(false);
  const [skillInstallResult, setSkillInstallResult] = useState(null);
  const [clawpkgDragOver, setClawpkgDragOver] = useState(false);
  const [clawpkgImporting, setClawpkgImporting] = useState(false);
  const [clawpkgImportResult, setClawpkgImportResult] = useState(null);
  const [localComponents, setLocalComponents] = useState([]);
  const [componentLibraryPath, setComponentLibraryPath] = useState("");
  const [componentLibraryMigration, setComponentLibraryMigration] = useState(null);
  const [componentLibraryLoading, setComponentLibraryLoading] = useState(false);
  const [pendingComponentAction, setPendingComponentAction] = useState(null);
  const [componentActionPending, setComponentActionPending] = useState(false);
  const [componentActionError, setComponentActionError] = useState("");
  const [deviceInventory, setDeviceInventory] = useState(
    () => readSessionDeviceInventory(liveInventoryTarget) || EMPTY_DEVICE_INVENTORY,
  );
  const [inventoryLoading, setInventoryLoading] = useState(false);
  const [libraryKind, setLibraryKind] = useState("all");
  const libraryRefreshRequestRef = useRef(0);
  const inventoryRefreshRequestRef = useRef(0);
  const componentActionDialogRef = useRef(null);
  const componentActionCancelRef = useRef(null);
  const componentActionReturnFocusRef = useRef(null);
  /* USB OTA install flow modal: idle | checking-usb | waiting-usb | installing | success | error */
  const [otaPhase, setOtaPhase] = useState("idle");
  const [otaPendingPath, setOtaPendingPath] = useState(null);
  const [otaPendingOptions, setOtaPendingOptions] = useState({});
  const [otaTargetName, setOtaTargetName] = useState("");
  const [otaError, setOtaError] = useState(null);
  const [otaResult, setOtaResult] = useState(null);

  useEffect(() => {
    setActiveComponentRecord(deviceCurrentComponent);
  }, [deviceCurrentComponent]);

  const refreshComponentLibrary = useCallback(async () => {
    const requestId = libraryRefreshRequestRef.current + 1;
    libraryRefreshRequestRef.current = requestId;
    setComponentLibraryLoading(true);
    try {
      const snapshot = await invoke("list_component_library");
      if (requestId === libraryRefreshRequestRef.current) {
        setLocalComponents(Array.isArray(snapshot?.components) ? snapshot.components : []);
        setComponentLibraryPath(String(snapshot?.libraryPath || ""));
        setComponentLibraryMigration(snapshot?.migration || null);
      }
    } catch (err) {
      console.warn("[ComponentCenter] list_component_library failed", err);
    } finally {
      if (requestId === libraryRefreshRequestRef.current) setComponentLibraryLoading(false);
    }
  }, []);

  /* Load immediately and keep a low-frequency fallback for platforms where
     native file notifications are unavailable or temporarily interrupted. */
  useEffect(() => {
    refreshComponentLibrary();
    const interval = setInterval(refreshComponentLibrary, 30000);
    return () => clearInterval(interval);
  }, [refreshComponentLibrary]);

  useEffect(() => {
    if (!componentLibraryPath) return undefined;
    let disposed = false;
    let stopWatching = null;
    let refreshTimer = null;
    watch(
      componentLibraryPath,
      () => {
        if (disposed) return;
        window.clearTimeout(refreshTimer);
        refreshTimer = window.setTimeout(refreshComponentLibrary, 250);
      },
      { recursive: true },
    ).then((unwatch) => {
      if (disposed) unwatch();
      else stopWatching = unwatch;
    }).catch((error) => {
      console.warn("[ComponentCenter] component library watch unavailable", error);
    });
    return () => {
      disposed = true;
      window.clearTimeout(refreshTimer);
      stopWatching?.();
    };
  }, [componentLibraryPath, refreshComponentLibrary]);

  const refreshDeviceInventory = useCallback(async ({
    silent = false,
    announce = false,
  } = {}) => {
    const requestId = inventoryRefreshRequestRef.current + 1;
    inventoryRefreshRequestRef.current = requestId;
    const target = liveInventoryTarget;
    if (!target) {
      setDeviceInventory(EMPTY_DEVICE_INVENTORY);
      setInventoryLoading(false);
      if (announce) {
        push({
          tone: "warning",
          title: "尚未连接目标设备",
          message: "连接 USB 板端或配置 SSH 主机后再同步组件。",
        });
      }
      return EMPTY_DEVICE_INVENTORY;
    }
    if (!silent) setInventoryLoading(true);
    try {
      const nextInventory = await requestSessionDeviceInventory(target);
      if (requestId !== inventoryRefreshRequestRef.current) return null;
      setDeviceInventory(nextInventory);
      if (announce) {
        push({
          tone: nextInventory.freshness === "live"
            ? "success"
            : nextInventory.freshness === "unsupported"
              ? "warning"
              : "error",
          title: nextInventory.freshness === "live"
            ? `已同步 ${nextInventory.items.length} 个板端组件`
            : nextInventory.freshness === "unsupported"
              ? "板端固件缺少清单能力"
              : "同步板端组件失败",
          message: nextInventory.freshness === "live"
            ? nextInventory.activeWidgetId
              ? `当前启用：${nextInventory.activeWidgetId}`
              : "板端当前没有启用组件。"
            : nextInventory.warnings[0],
        });
      }
      return nextInventory;
    } finally {
      if (requestId === inventoryRefreshRequestRef.current) setInventoryLoading(false);
    }
  }, [liveInventoryTarget, push]);

  useEffect(() => {
    const cachedInventory = readSessionDeviceInventory(liveInventoryTarget);
    if (cachedInventory) {
      inventoryRefreshRequestRef.current += 1;
      setDeviceInventory(cachedInventory);
      setInventoryLoading(false);
      return;
    }
    refreshDeviceInventory();
  }, [liveInventoryTarget, refreshDeviceInventory]);

  useEffect(() => {
    if (typeof window === "undefined" || !window.localStorage) return;
    try {
      window.localStorage.setItem(
        COMPONENT_BUTTON_OVERRIDES_STORAGE_KEY,
        JSON.stringify(bindingOverrides),
      );
    } catch (err) {
      console.warn("[ComponentCenter] failed to persist component button overrides", err);
    }
  }, [bindingOverrides]);

  /** Build the shared preview/install shape from a formally published entry. */
  function buildLibraryComponent(entry) {
    return {
      id: entry.id,
      name: entry.name || entry.id,
      goal: buildLibraryGoal(entry),
      dashboard: entry.dashboard || {},
      gameType: entry.gameType || "",
      runtimeEngine: entry.runtimeEngine || "",
      sceneEngine: entry.sceneEngine || "",
      gamePreset: entry.gamePreset || "",
      scene: entry.scene || null,
      kind: resolveComponentKind(entry.kind, entry.gameType),
      defaultBindings: Array.isArray(entry.buttons) ? entry.buttons.filter(isRoutedWidgetBinding) : [],
      screens: [{ name: "负一屏", purpose: "正式本地组件自带独立按钮功能绑定", regions: [] }],
      status: "library",
      path: entry.path,
      libraryPath: entry.path,
      versionHash: entry.versionHash || "",
      isLocal: true,
      createdAtMs: entry.createdAtMs || entry.mtimeMs || 0,
      valid: entry.valid !== false,
      validationErrors: Array.isArray(entry.validationErrors)
        ? entry.validationErrors
        : [],
    };
  }

  /** Resolve the exact installed source. A generated local component may intentionally
   *  share its manifest id with a builtin, so id alone is not enough. */
  const currentFullComponent = useMemo(() => {
    const record = activeComponentRecord;
    const id = record?.id;
    if (!id) return null;
    if (record?.source?.type === "library" && record.source.path) {
      const exactComponent = localComponents.find(
        (component) => normalizeLocalPath(component.path) === normalizeLocalPath(record.source.path),
      );
      if (exactComponent) return buildLibraryComponent(exactComponent);
      // Keep the installed component's exact source identity even if its local
      // files were removed outside Component Center. Falling through to a
      // builtin with the same id would mislabel the hero and block a safe,
      // target-bound device removal.
      return {
        ...record,
        path: record.source.path,
        libraryPath: record.source.path,
        isLocal: true,
      };
    }
    if (record?.source?.type === "draft") {
      const migrated = localComponents.find((component) => component.id === id);
      if (migrated) return buildLibraryComponent(migrated);
    }
    const builtin = BUILTIN_COMPONENT_CENTER.components.find((c) => c.id === id);
    if (builtin) return builtin;
    const local = localComponents.find((component) => component.id === id);
    if (local) return buildLibraryComponent(local);
    // External / unknown: return minimal shape so hero can still render name
    return record;
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeComponentRecord, localComponents]);
  const activeTargetVerified = activeComponentRecord?.targetVerified === true;
  const enabledComponentSourceKey = activeTargetVerified
    ? activeRecordSourceKey(activeComponentRecord)
    : "";

  /** Builtins keep the product-defined order; generated packages stay newest-first. */
  const catalogItems = useMemo(() => {
    const builtins = BUILTIN_COMPONENT_CENTER.components.map((item) => ({
      ...item,
      kind: resolveComponentKind(item.kind, item.gameType),
      isLocal: false,
    }));
    const publishedItems = localComponents.map((entry) => buildLibraryComponent(entry));
    return [...builtins, ...sortComponentsByCreatedAt(publishedItems)];
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [localComponents]);

  /** Packages reported by the board but absent from this PC are folded into the
   *  same grid. They remain removable, but cannot be installed or edited locally. */
  const libraryItems = useMemo(() => {
    if (deviceInventory.freshness !== "live") return catalogItems;
    const catalogIds = new Set(catalogItems.map((item) => item.id));
    const deviceOnlyItems = deviceInventory.items
      .filter((item) => !catalogIds.has(item.id))
      .map((item) => buildUnknownInventoryComponent(item));
    return [...catalogItems, ...deviceOnlyItems];
  }, [catalogItems, deviceInventory.freshness, deviceInventory.items]);

  /* A successful live inventory is the device truth. Reconcile the small
     target-scoped local record so ContextRail and the library marker follow
     what the board actually reports, while preserving an exact local source
     when the active id has not changed. */
  useEffect(() => {
    if (deviceInventory.freshness !== "live" || !liveInventoryTarget) return;
    const targetKey = activeComponentTargetKey(liveInventoryTarget);
    const recordTargetKey = activeComponentTargetKey(activeComponentRecord?.target);
    const activeId = deviceInventory.activeWidgetId;
    if (!activeId) {
      if (activeComponentRecord?.targetVerified && recordTargetKey === targetKey) {
        clearActiveComponentState(liveInventoryTarget);
      }
      return;
    }
    if (
      activeComponentRecord?.targetVerified
      && recordTargetKey === targetKey
      && activeComponentRecord.id === activeId
    ) {
      return;
    }
    const inventoryItem = deviceInventory.items.find((item) => item.id === activeId);
    const component = libraryItems.find((item) => item.id === activeId)
      || buildUnknownInventoryComponent(inventoryItem || { id: activeId, active: true });
    try {
      const record = writeActiveComponentForTarget(component, liveInventoryTarget);
      setActiveComponentRecord(record);
      window.dispatchEvent(new Event("storage"));
    } catch (error) {
      console.warn("[ComponentCenter] failed to reconcile live component inventory", error);
    }
  // clearActiveComponentState is a function declaration and remains stable for this render.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [
    activeComponentRecord,
    deviceInventory.activeWidgetId,
    deviceInventory.freshness,
    deviceInventory.items,
    libraryItems,
    liveInventoryTarget,
  ]);

  const installedIds = useMemo(
    () => new Set(deviceInventory.items.map((item) => item.id)),
    [deviceInventory.items],
  );
  const filteredLibraryItems = useMemo(
    () => libraryItems.filter((item) => {
      const kind = resolveComponentKind(item.kind, item.gameType);
      return libraryKind === "all" || kind === libraryKind;
    }),
    [libraryItems, libraryKind],
  );

  const librarySectionRef = useRef(null);
  const previewIsInstalled = Boolean(
    previewComponent
    && deviceInventory.freshness === "live"
    && installedIds.has(previewComponent.id),
  );
  const previewInventoryItem = previewComponent
    ? deviceInventory.items.find((item) => item.id === previewComponent.id) || null
    : null;
  const previewIsCurrent = Boolean(
    previewComponent
    && activeTargetVerified
    && componentSourceKey(previewComponent) === enabledComponentSourceKey,
  );
  const previewReplacesSingleSlot = Boolean(
    previewComponent
    && !previewComponent.isDeviceOnly
    && deviceInventory.supportsMultiple === false
    && currentFullComponent
    && !previewIsCurrent,
  );

  function focusComponentLibrary() {
    const section = librarySectionRef.current;
    if (!section) return;
    const reducedMotion = window.matchMedia?.("(prefers-reduced-motion: reduce)")?.matches;
    section.scrollIntoView({ behavior: reducedMotion ? "auto" : "smooth", block: "start" });
    window.setTimeout(
      () => section.querySelector('.component-library-filters button[aria-pressed="true"]')?.focus(),
      220,
    );
  }

  async function syncSelectedComponent(component) {
    if (!component?.id || component.isDeviceOnly) return;
    const validationError = localInstallBlockedReason(component);
    if (validationError) {
      push({
        tone: "error",
        title: "正式本地组件暂时不能同步",
        message: validationError,
      });
      return;
    }
    const firmwareError = gameInstallBlockedReason(component, usb);
    if (firmwareError) {
      push({
        tone: "error",
        title: `暂时无法同步 · ${component.name || component.id}`,
        message: firmwareError,
      });
      return;
    }
    if (!liveInventoryTarget) {
      push({
        tone: "warning",
        title: "尚未连接目标设备",
        message: "连接 USB 板端或配置 SSH 主机后再同步组件。",
      });
      return;
    }
    const maxInstalled = Number(deviceInventory.maxInstalled);
    if (
      deviceInventory.freshness === "live"
      && deviceInventory.supportsMultiple !== false
      && !installedIds.has(component.id)
      && Number.isFinite(maxInstalled)
      && maxInstalled > 0
      && deviceInventory.items.length >= maxInstalled
    ) {
      push({
        tone: "warning",
        title: `设备最多同步 ${maxInstalled} 个组件`,
        message: "请先在任一已同步组件卡片上点击移除，再同步新组件。",
      });
      return;
    }
    await installSelectedComponent(component);
  }

  async function installSelectedComponent(component = previewComponent) {
    if (!component) return false;
    setPreviewComponent(null);

    /* Formal local components ship their own buttons.json + dashboard. Builtins
       resolve through the Tauri backend so packaged apps use bundled resources
       instead of assuming ~/.openclaw exists on every user's machine. */
    const localPath = component.libraryPath || component.draftPath;
    if (localPath) {
      return installClawpkgFromPath(localPath, {
        targetName: component.name,
        skipFooterOverride: true,
        component,
      });
    }
    return installBuiltinToDevice(component);
  }

  function markComponentInstalled(component, target) {
    if (!component || !target) return;
    const record = writeActiveComponentForTarget(component, target);
    setActiveComponentRecord(record);
    window.dispatchEvent(new Event("storage"));
  }

  function requestDeleteLibraryComponent(component, installedOnDevice = false) {
    if (installedOnDevice && (deviceInventory.freshness !== "live" || !liveInventoryTarget)) {
      push({
        tone: "warning",
        title: "请先连接并读取设备清单",
        message: "双端删除会先等待设备确认，不能在设备目标不明确时只删除电脑副本。",
      });
      return;
    }
    const inventoryItem = installedOnDevice
      ? deviceInventory.items.find((item) => item.id === component.id)
      : null;
    componentActionReturnFocusRef.current = document.activeElement;
    setPendingComponentAction({
      type: "delete-library",
      component,
      installedOnDevice,
      target: installedOnDevice ? liveInventoryTarget : null,
      wasActive: inventoryItem?.active === true,
    });
    setComponentActionError("");
  }

  function requestRemoveInventoryItem(item) {
    if (!item?.id || deviceInventory.freshness !== "live" || !liveInventoryTarget) {
      push({
        tone: "warning",
        title: "请先同步板端组件",
        message: "只有设备返回实时清单后，才能安全删除指定组件。",
      });
      return;
    }
    componentActionReturnFocusRef.current = document.activeElement;
    setPendingComponentAction({
      type: "remove-device",
      component: item.component || {
        id: item.id,
        name: item.name || item.id,
        kind: item.kind || "tool",
      },
      target: liveInventoryTarget,
      wasActive: item.active === true,
      originSlot: item.slot,
    });
    setComponentActionError("");
  }

  function restoreComponentActionFocus() {
    window.setTimeout(() => {
      const previous = componentActionReturnFocusRef.current;
      if (previous?.isConnected && typeof previous.focus === "function") {
        previous.focus();
      } else {
        librarySectionRef.current?.querySelector("button")?.focus();
      }
    }, 0);
  }

  function cancelComponentAction() {
    if (componentActionPending) return;
    setPendingComponentAction(null);
    setComponentActionError("");
    restoreComponentActionFocus();
  }

  useEffect(() => {
    if (!pendingComponentAction) return undefined;
    componentActionCancelRef.current?.focus();
    const handleKeyDown = (event) => {
      if (event.key === "Escape" && !componentActionPending) {
        event.preventDefault();
        cancelComponentAction();
        return;
      }
      if (event.key !== "Tab") return;
      const focusable = Array.from(
        componentActionDialogRef.current?.querySelectorAll(
          'button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])',
        ) || [],
      );
      if (focusable.length === 0) return;
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    };
    document.addEventListener("keydown", handleKeyDown);
    return () => document.removeEventListener("keydown", handleKeyDown);
  }, [componentActionPending, pendingComponentAction]);

  function clearActiveComponentState(target) {
    setActiveComponentRecord(null);
    try {
      removeActiveComponentForTarget(target);
      window.dispatchEvent(new Event("storage"));
    } catch (err) {
      console.warn("[ComponentCenter] failed to clear active component", err);
    }
  }

  function clearComponentBindingOverrides(component) {
    const keyPrefix = `${componentSourceKey(component)}:`;
    if (!component?.id || !keyPrefix) return;
    setBindingOverrides((current) => Object.fromEntries(
      Object.entries(current).filter(([key]) => !key.startsWith(keyPrefix)),
    ));
  }

  async function confirmComponentAction() {
    if (!pendingComponentAction?.component) return;
    const {
      type,
      component,
      target,
      wasActive = false,
      installedOnDevice = false,
    } = pendingComponentAction;
    setComponentActionPending(true);
    setComponentActionError("");
    try {
      const removesDevice = type === "remove-device" || installedOnDevice;
      let deviceResult = null;
      if (removesDevice) {
        if (!target) throw new Error("无法确认要删除组件的目标设备");
        deviceResult = await invoke("remove_widget_from_device", {
          input: {
            componentId: component.id,
            transport: target.transport,
            boardDeviceId: target.boardDeviceId || "",
            sshHost: target.sshHost || "",
          },
        });
        if (deviceResult && typeof deviceResult === "object" && deviceResult.ok === false) {
          throw new Error(deviceResult.error || deviceResult.message || "设备拒绝移除组件");
        }
        if (wasActive) clearActiveComponentState(target);
        setDeviceInventory((current) => writeSessionDeviceInventory(target, {
          ...current,
          activeWidgetId: wasActive ? "" : current.activeWidgetId,
          items: current.items.filter((item) => item.id !== component.id),
        }));
      }

      if (type === "delete-library") {
        // Device removal happens first. A failed board ACK leaves the local
        // package intact, so users never lose the only source by accident.
        const targetPath = component.libraryPath || component.path;
        if (!targetPath) throw new Error("找不到要删除的正式本地组件路径");
        await invoke("delete_component_from_library", { input: { path: targetPath } });
        setLocalComponents((current) => current.filter(
          (entry) => normalizeLocalPath(entry.path) !== normalizeLocalPath(targetPath),
        ));
        clearComponentBindingOverrides(component);
        if (
          previewComponent?.id === component.id
          && normalizeLocalPath(previewComponent.libraryPath) === normalizeLocalPath(targetPath)
        ) {
          setPreviewComponent(null);
        }
        await refreshComponentLibrary();
        push({
          tone: deviceResult?.warning ? "warning" : "success",
          title: installedOnDevice
            ? `已从电脑和设备删除 · ${component.name || component.id}`
            : `已从电脑删除 · ${component.name || component.id}`,
          message: deviceResult?.warning || undefined,
        });
      } else {
        setPreviewComponent(null);
        push({
          tone: deviceResult?.warning ? "warning" : "success",
          title: `已从设备移除 · ${component.name || component.id}`,
          message: deviceResult?.warning || undefined,
        });
      }
      if (removesDevice) await refreshDeviceInventory({ silent: true });
      setPendingComponentAction(null);
      restoreComponentActionFocus();
    } catch (err) {
      setComponentActionError(componentActionErrorMessage(err));
    } finally {
      setComponentActionPending(false);
    }
  }

  function resolveControlOption(binding, component) {
    const key = bindingKey(component, binding.action);
    const legacyKey = `${component?.id || ""}:${binding.action}`;
    const label = bindingOverrides[key]
      || bindingOverrides[legacyKey]
      || defaultControlLabelForBinding(binding);
    return optionForControlLabel(label) || {
      label,
      shortLabel: label,
      control: binding.control || label,
      event: binding.event || "",
      help: CONTROL_HELP[label] || "安装时会把这个组件动作写入 buttons.json。",
    };
  }

  function resolveComponentBindings(component) {
    if (!component || !Array.isArray(component.defaultBindings)) return [];
    return component.defaultBindings
      .filter(isRoutedWidgetBinding)
      .map((binding) => {
        const option = resolveControlOption(binding, component);
        return {
          ...binding,
          control: option.control,
          event: option.event,
          controlLabel: option.label,
          controlHelp: option.help,
        };
      });
  }

  function bindingConflictForComponent(component) {
    const seen = new Map();
    for (const binding of resolveComponentBindings(component)) {
      if (binding.event.startsWith("screen.") && !deviceTouchReady(usb)) {
        return `${binding.label} 使用了屏幕手势，但当前设备未报告触屏可用，请改为 SW1/SW2/SW3 或旋钮`;
      }
      for (const eventSlot of componentInputEventSlots(binding.event)) {
        if (seen.has(eventSlot)) {
          return `${seen.get(eventSlot)} 和 ${binding.label} 都绑定到了 ${binding.controlLabel}`;
        }
        seen.set(eventSlot, binding.label);
      }
    }
    return "";
  }

  function buildBindingOverridesForInstall(component) {
    if (!component || !Array.isArray(component.defaultBindings)) return {};
    return component.defaultBindings.reduce((overrides, binding) => {
      const selectedLabel = bindingOverrides[bindingKey(component, binding.action)]
        || bindingOverrides[`${component.id}:${binding.action}`];
      if (selectedLabel && selectedLabel !== defaultControlLabelForBinding(binding)) {
        overrides[binding.action] = selectedLabel;
      }
      return overrides;
    }, {});
  }

  function updateBinding(binding, nextControl, component) {
    setBindingOverrides((current) => ({
      ...current,
      [bindingKey(component, binding.action)]: nextControl,
    }));
  }

  function controlOptionsForBinding(binding, component) {
    const currentEvent = resolveControlOption(binding, component).event;
    const usedEvents = new Set(
      resolveComponentBindings(component)
        .filter((candidate) => candidate.action !== binding.action)
        .flatMap((candidate) => componentInputEventSlots(candidate.event)),
    );
    return COMPONENT_CONTROL_OPTIONS
      .filter((option) => (
        !option.event.startsWith("screen.")
        || deviceTouchReady(usb)
        || option.event === currentEvent
      ))
      .map((option) => ({
        ...option,
        disabled: (
          (option.event.startsWith("screen.") && !deviceTouchReady(usb))
          || (
            option.event !== currentEvent
            && componentInputEventSlots(option.event).some((event) => usedEvents.has(event))
          )
        ),
      }));
  }

  function resetBindings(component) {
    setBindingOverrides((current) => {
      const next = { ...current };
      (component?.defaultBindings || []).forEach((binding) => {
        delete next[bindingKey(component, binding.action)];
      });
      return next;
    });
  }

  function buildBindingsFooter(component) {
    if (!component || !Array.isArray(component.defaultBindings)) return "";
    return component.defaultBindings
      .filter(isRoutedWidgetBinding)
      .map((binding) => {
        const option = resolveControlOption(binding, component);
        return `${option.shortLabel || option.label} ${binding.label}`;
      })
      .slice(0, 3)
      .join(" · ");
  }

  async function startOtaInstall(componentId, clawpkgPath, options = {}) {
    const component = BUILTIN_COMPONENT_CENTER.components.find((c) => c.id === componentId);
    const targetName = options.targetName || component?.name || componentId;
    setOtaTargetName(targetName);
    setOtaPendingPath(clawpkgPath);
    setOtaPendingOptions(options);
    setOtaError(null);
    setOtaResult(null);
    const sshHost = readConfiguredComponentSshHost();
    if (sshHost && !usb.connected && !options.forceUsb) {
      return performOtaInstall(componentId, clawpkgPath, options);
    }
    setOtaPhase("checking-usb");
    let status;
    try {
      status = await invoke("usb_get_status");
    } catch (err) {
      // USB state is owned by useDeviceContext
    }
    if (!status?.connected && !deviceConnected) {
      setOtaPhase("waiting-usb");
      return false;
    }
    return performOtaInstall(componentId, clawpkgPath, options);
  }

  async function performOtaInstall(componentId, clawpkgPath, options = {}) {
    const component = options.component
      || BUILTIN_COMPONENT_CENTER.components.find((c) => c.id === componentId)
      || (() => {
        const local = localComponents.find(
          (item) => item.id === componentId || matchesLibraryPath(item, clawpkgPath),
        );
        return local ? buildLibraryComponent(local) : null;
      })();
    setOtaPhase("installing");

    /* Bind installation to one explicit target. A live USB board wins over a
       remembered SSH host, preventing stale LAN settings from silently writing
       another device. SSH remains the offline-board transport. */
    const sshHost = readConfiguredComponentSshHost();
    let componentPackageInstalled = false;
    let attemptedSsh = false;
    try {
      let result;
      let installTarget;
      let liveStatus = null;
      try {
        liveStatus = await invoke("usb_get_status");
      } catch (err) {
        // The install path below reports the actionable transport error.
      }
      const liveUsbConnected = Boolean(liveStatus?.connected);
      const useSsh = !options.forceUsb && !liveUsbConnected && sshHost.length > 0;
      attemptedSsh = useSsh;
      if (useSsh) {
        installTarget = { transport: "ssh", sshHost };
        result = await invoke("install_clawpkg_over_ssh", {
          input: {
            clawpkgPath,
            sshHost,
            bindingOverrides: buildBindingOverridesForInstall(component),
          },
        });
      } else {
        const boardDeviceId = String(
          liveStatus?.boardDeviceId || usb.boardDeviceId || "",
        ).trim();
        if (!liveStatus?.connected || !boardDeviceId) {
          throw new Error("USB 设备尚未完成身份握手，无法安全记录组件目标。");
        }
        installTarget = { transport: "usb", boardDeviceId };
        const footerOverride = options.skipFooterOverride ? "" : buildBindingsFooter(component);
        result = await invoke("install_clawpkg_over_usb", {
          input: {
            clawpkgPath,
            footerOverride,
            bindingOverrides: buildBindingOverridesForInstall(component),
          },
        });
      }
      if (!result.ok) {
        const validationError = `校验失败: ${result.errors.join("; ")}`;
        setOtaError(validationError);
        setOtaPhase("error");
        push({ tone: "error", title: "安装失败", message: result.errors.join("; ") });
        return false;
      }
      componentPackageInstalled = true;
      markComponentInstalled(component, installTarget);
      await refreshDeviceInventory({ silent: true });
      setOtaResult(result);
      setOtaPhase("success");
      push({
        tone: "success",
        title: `已同步到设备 · ${result.manifest?.name || otaTargetName}`,
      });
      return true;
    } catch (err) {
      const detail = typeof err === "string" ? err : String(err);
      const msg = detail;
      if (
        !componentPackageInstalled
        && !attemptedSsh
        && (msg.includes("USB 未连接") || msg.includes("USB not connected"))
      ) {
        setOtaPhase("waiting-usb");
      } else {
        setOtaError(msg);
        setOtaPhase("error");
        push({
          tone: "error",
          title: componentPackageInstalled ? "组件已安装，本地状态更新失败" : "同步失败",
          message: msg,
        });
      }
      return false;
    }
  }

  async function installBuiltinToDevice(component) {
    const id = component?.id || component;
    const clawpkgPath = await invoke("resolve_builtin_clawpkg_path", { id });
    return startOtaInstall(id, clawpkgPath, { component });
  }

  /* poll USB status every 2s while modal is waiting; auto-retry when connected */
  useEffect(() => {
    if (otaPhase !== "waiting-usb" || !otaPendingPath) return undefined;
    let cancelled = false;
    let triggered = false;
    const tick = async () => {
      try {
        const status = await invoke("usb_get_status");
        const ok = Boolean(status?.connected);
        if (cancelled) return;
        if (ok && !triggered) {
          triggered = true;
          clearInterval(interval);
          const localId = localComponents.find(
            (component) => matchesLibraryPath(component, otaPendingPath),
          )?.id;
          const builtinId = BUILTIN_COMPONENT_CENTER.components.find(
            (c) => otaPendingPath && otaPendingPath.includes(c.id),
          )?.id;
          await performOtaInstall(localId || builtinId || "", otaPendingPath, otaPendingOptions);
          if (cancelled) return;
        }
      } catch (err) {
        // USB state is owned by useDeviceContext
      }
    };
    const interval = setInterval(tick, 2000);
    tick();
    return () => { cancelled = true; clearInterval(interval); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [otaPhase, otaPendingPath]);

  function dismissOtaModal() {
    setOtaPhase("idle");
    setOtaPendingPath(null);
    setOtaError(null);
    setOtaResult(null);
  }

  async function handleInstallSkill() {
    setSkillInstalling(true);
    try {
      const result = await invoke("install_widget_skill");
      setSkillInstallResult(result);
      const installedCount = result?.installed?.length ?? 0;
      push({
        tone: installedCount > 0 ? "success" : "info",
        title: installedCount > 0
          ? `Skill 已安装到 ${installedCount} 个 coding agent`
          : "未检测到可安装的 coding agent",
      });
    } catch (err) {
      const msg = typeof err === "string" ? err : String(err);
      push({ tone: "error", title: "Skill 安装失败", message: msg });
    } finally {
      setSkillInstalling(false);
    }
  }

  async function installClawpkgFromPath(clawpkgPath, options = {}) {
    setClawpkgImporting(true);
    try {
      const localMatch = localComponents.find((component) => matchesLibraryPath(component, clawpkgPath));
      const builtinMatch = BUILTIN_COMPONENT_CENTER.components.find((c) => clawpkgPath.includes(c.id));
      const guessedId = localMatch?.id || builtinMatch?.id || currentFullComponent?.id || "";
      const resolvedOptions = localMatch
        ? {
            targetName: localMatch.name,
            skipFooterOverride: true,
            component: buildLibraryComponent(localMatch),
            ...options,
          }
        : options;
      return await startOtaInstall(guessedId, clawpkgPath, resolvedOptions);
    } catch (err) {
      const msg = typeof err === "string" ? err : String(err);
      push({ tone: "error", title: "安装 .clawpkg 失败", message: msg });
      return false;
    } finally {
      setClawpkgImporting(false);
    }
  }

  async function importClawpkgToLibrary(clawpkgPath) {
    setClawpkgImporting(true);
    try {
      const published = await invoke("import_component_to_library", {
        input: { path: clawpkgPath },
      });
      const component = buildLibraryComponent(published);
      setClawpkgImportResult({
        manifest: { name: component.name },
        publishedPath: component.libraryPath,
      });
      await refreshComponentLibrary();
      setCreateDrawerOpen(false);
      setPreviewComponent(component);
    } catch (err) {
      const msg = typeof err === "string" ? err : String(err);
      push({ tone: "error", title: "导入正式组件库失败", message: msg });
    } finally {
      setClawpkgImporting(false);
    }
  }

  async function handleClawpkgDrop(event) {
    event.preventDefault();
    setClawpkgDragOver(false);
    const file = event.dataTransfer.files && event.dataTransfer.files[0];
    if (!file) { push({ tone: "error", title: "没有读到文件" }); return; }
    const localPath = file.path || file.webkitRelativePath;
    if (!localPath) {
      push({
        tone: "error",
        title: "无法获取本地路径",
        message: "浏览器模式下拖拽不支持获取真实路径,请用 Tauri 桌面模式或'选择文件'按钮。",
      });
      return;
    }
    await importClawpkgToLibrary(localPath);
  }

  async function handleClawpkgFilePick() {
    try {
      const selectedPath = await openDialog({
        multiple: false,
        directory: false,
        filters: [{ name: "petui 组件", extensions: ["clawpkg", "zip"] }],
      });
      if (typeof selectedPath === "string" && selectedPath) {
        await importClawpkgToLibrary(selectedPath);
      }
    } catch (error) {
      push({
        tone: "error",
        title: "选择组件包失败",
        message: typeof error === "string" ? error : String(error),
      });
    }
  }

  return (
    <PageShell
      title="组件中心"
      help={onboarding.show}
      actions={[
        <button
          key="refresh-library"
          type="button"
          className="btn-ghost btn-sm"
          onClick={() => {
            refreshComponentLibrary();
            refreshDeviceInventory({ silent: true });
          }}
          disabled={componentLibraryLoading || inventoryLoading}
        >
          <RefreshCw size={14} />
          {componentLibraryLoading || inventoryLoading ? "刷新中…" : "刷新组件库"}
        </button>,
        <button
          key="open-create-drawer"
          type="button"
          className="btn-primary btn-sm"
          onClick={() => setCreateDrawerOpen(true)}
        >
          <Sparkles size={14} />
          创建组件
        </button>,
      ]}
    >
      <PageOnboardingModal
        id="component-center"
        open={onboarding.open}
        onClose={onboarding.dismiss}
        title="添加或创建小组件，只要三步"
        description="组件库卡片会直接显示设备同步状态，不再需要单独管理板端列表。"
        steps={[
          {
            title: "浏览或创建",
            description: "从组件库选择，或在当前 Agent 对话中调用 petui 生成新组件。",
          },
          {
            title: "预览与配置",
            description: "打开组件查看画面并确认按键设置。",
          },
          {
            title: "直接同步",
            description: "未同步组件可直接下发；已同步组件可从同一张卡片移除。",
          },
        ]}
        actions={[
          {
            label: "浏览组件库",
            onClick: () => {
              focusComponentLibrary();
            },
          },
          {
            label: "创建组件",
            variant: "primary",
            icon: <Sparkles size={14} />,
            onClick: () => {
              setCreateDrawerOpen(true);
            },
          },
        ]}
      />

      <div className="component-center-workspace">
      {/* ── one library: local availability and live board state ─────────── */}
      <section
        ref={librarySectionRef}
        id="component-library"
        className="component-library-section"
      >
        <div className="component-library-toolbar">
          <div className="component-library-filters" aria-label="按组件类型筛选">
            {[
              { id: "all", label: "全部", icon: LayoutGrid },
              { id: "game", label: "小游戏", icon: Gamepad2 },
              { id: "tool", label: "工具", icon: Wrench },
            ].map((filter) => {
              const Icon = filter.icon;
              return (
                <button
                  type="button"
                  className={libraryKind === filter.id ? "is-active" : ""}
                  aria-pressed={libraryKind === filter.id}
                  onClick={() => setLibraryKind(filter.id)}
                  key={filter.id}
                >
                  <Icon size={13} aria-hidden="true" />
                  {filter.label}
                </button>
              );
            })}
          </div>
        </div>

        <div className="component-library-grid">
          {filteredLibraryItems.map((item) => {
            const isEnabled = Boolean(
              deviceInventory.freshness === "live"
              && deviceInventory.activeWidgetId === item.id
              && enabledComponentSourceKey
              && componentSourceKey(item) === enabledComponentSourceKey,
            );
            const isInstalled = deviceInventory.freshness === "live" && installedIds.has(item.id);
            const inventoryItem = isInstalled
              ? deviceInventory.items.find((entry) => entry.id === item.id)
              : null;
            return (
              <CandidateCard
                key={item.isLocal ? item.libraryPath || item.id : item.id}
                component={item}
                kind={resolveComponentKind(item.kind, item.gameType)}
                isLocal={item.isLocal}
                isInstalled={isInstalled}
                isEnabled={isEnabled}
                onClick={() => setPreviewComponent(item)}
                onDeviceAction={() => {
                  if (isInstalled) {
                    requestRemoveInventoryItem({
                      ...inventoryItem,
                      id: item.id,
                      name: item.name,
                      kind: item.kind,
                      component: item,
                    });
                  } else {
                    setPreviewComponent(item);
                  }
                }}
                onDelete={
                  item.isLocal
                    ? () => requestDeleteLibraryComponent(item, isInstalled)
                    : undefined
                }
              />
            );
          })}
          <CreateNewCard onClick={() => setCreateDrawerOpen(true)} />
        </div>
        {filteredLibraryItems.length === 0 && (
          <div className="component-library-empty" role="status">
            <LayoutGrid size={22} aria-hidden="true" />
            <strong>该分类暂无组件</strong>
            <span>切换“全部 / 小游戏 / 工具”查看其他组件。</span>
          </div>
        )}
      </section>

      {/* ── modals ──────────────────────────────────────────────────────── */}

      {previewComponent && (
        <ComponentPreviewModal
          component={previewComponent}
          kind={resolveComponentKind(previewComponent.kind, previewComponent.gameType)}
          isLocal={previewComponent.isLocal}
          isInstalled={previewIsInstalled}
          currentComponent={currentFullComponent}
          isCurrent={previewIsCurrent}
          singleSlotReplacement={previewReplacesSingleSlot}
          deviceConnected={deviceConnected}
          installing={otaPhase === "installing"}
          bindings={resolveComponentBindings(previewComponent)}
          bindingConflict={bindingConflictForComponent(previewComponent)}
          getControlOptions={(binding) => controlOptionsForBinding(binding, previewComponent)}
          onBindingChange={(binding, nextControl) => updateBinding(binding, nextControl, previewComponent)}
          onResetBindings={() => resetBindings(previewComponent)}
          componentButtonsWillApply={
            Array.isArray(previewComponent.defaultBindings)
            && previewComponent.defaultBindings.filter(isRoutedWidgetBinding).length > 0
          }
          installBlockedReason={
            previewComponent.isDeviceOnly
              ? "本机没有这个组件的安装源；可以继续保留，或从当前卡片移除。"
              : localInstallBlockedReason(previewComponent)
                || gameInstallBlockedReason(previewComponent, usb)
          }
          onInstall={
            previewComponent.isDeviceOnly
              ? undefined
              : () => syncSelectedComponent(previewComponent)
          }
          onRemove={
            previewIsInstalled && previewInventoryItem
              ? () => {
                  const component = previewComponent;
                  setPreviewComponent(null);
                  requestRemoveInventoryItem({
                    ...previewInventoryItem,
                    id: component.id,
                    name: component.name,
                    kind: component.kind,
                    component,
                  });
                }
              : undefined
          }
          onDelete={
            previewComponent.isLocal
              ? () => {
                  const component = previewComponent;
                  setPreviewComponent(null);
                  requestDeleteLibraryComponent(component, previewIsInstalled);
                }
              : undefined
          }
          onClose={() => setPreviewComponent(null)}
        />
      )}

      {otaPhase !== "idle" && (
        <div className="component-replace-modal" role="dialog" aria-modal="true" aria-label="USB 同步到设备">
          <section className="ota-modal">
            <span className="ota-modal__eyebrow">
              {otaPhase === "success" ? "✓ 安装完成" : otaPhase === "error" ? "× 安装失败" : "USB OTA 安装"}
            </span>
            <h2>{otaTargetName}</h2>
            {otaPhase === "checking-usb" && <p>正在检测 USB 连接…</p>}
            {otaPhase === "waiting-usb" && (
              <>
                <p className="ota-modal__hint">
                  请用 <strong>数据线</strong> 把桌搭子连到电脑(注意要数据线,不能只供电)。
                  连上后会自动开始推送。
                </p>
                <div className="ota-modal__usb-status">
                  <span className={`ota-modal__dot ${usb.connected ? "is-on" : "is-off"}`} />
                  {usb.connected ? "USB 已连接,准备推送…" : "等待 USB 连接…"}
                </div>
              </>
            )}
            {otaPhase === "installing" && (
              <>
                <p>正在校验 .clawpkg + 通过 USB 串口推送 COMPONENT_DASHBOARD_V1 payload 到设备…</p>
                <div className="ota-modal__progress"><div /></div>
              </>
            )}
            {otaPhase === "success" && otaResult && (
              <p>
                已推送 <strong>{otaResult.transferredBytes}</strong> bytes 到设备负一屏。
                {otaResult.manifest?.name && ` 屏幕应已切到 "${otaResult.manifest.name}"。`}
              </p>
            )}
            {otaPhase === "error" && (
              <p className="ota-modal__error">{otaError || "未知错误"}</p>
            )}
            <div className="ota-modal__actions">
              {(otaPhase === "waiting-usb" || otaPhase === "checking-usb") && (
                <button className="btn-secondary" type="button" onClick={dismissOtaModal}>
                  取消
                </button>
              )}
              {otaPhase === "error" && otaPendingPath && (
                <button
                  className="btn-primary"
                  type="button"
                  onClick={() => {
                    const localId = localComponents.find(
                      (component) => matchesLibraryPath(component, otaPendingPath),
                    )?.id;
                    const builtinId = BUILTIN_COMPONENT_CENTER.components.find(
                      (c) => otaPendingPath.includes(c.id),
                    )?.id;
                    performOtaInstall(localId || builtinId || "", otaPendingPath, otaPendingOptions);
                  }}
                >
                  重试
                </button>
              )}
              {otaPhase === "success" && (
                <button
                  className="btn-secondary"
                  type="button"
                  title="设备负一屏切回宠物主屏（widget 仍保留在 widgets/<id>/）"
                  onClick={async () => {
                    try {
                      await invoke("usb_set_screen_page", { page: "main" });
                      push({ tone: "success", title: "已请求设备切回主屏" });
                      dismissOtaModal();
                    } catch (err) {
                      const msg = typeof err === "string" ? err : String(err);
                      push({ tone: "error", title: "切回主屏失败", message: msg });
                    }
                  }}
                >
                  ⤴ 返回主屏
                </button>
              )}
              {(otaPhase === "success" || otaPhase === "error") && (
                <button
                  className={otaPhase === "success" ? "btn-primary" : "btn-secondary"}
                  type="button"
                  onClick={dismissOtaModal}
                >
                  关闭
                </button>
              )}
            </div>
          </section>
        </div>
      )}

      {pendingComponentAction && (
        <div
          ref={componentActionDialogRef}
          className="component-replace-modal"
          role="alertdialog"
          aria-modal="true"
          aria-labelledby="component-action-confirm-title"
          aria-describedby="component-action-confirm-description"
        >
          <section className="component-action-confirm">
            <span>
              {pendingComponentAction.type === "remove-device"
                ? "设备操作"
                : pendingComponentAction.installedOnDevice
                  ? "电脑与设备"
                  : "电脑组件库"}
            </span>
            <h2 id="component-action-confirm-title">
              {pendingComponentAction.type === "remove-device"
                ? `从设备移除“${pendingComponentAction.component.name || pendingComponentAction.component.id}”？`
                : pendingComponentAction.installedOnDevice
                  ? `从电脑和设备删除“${pendingComponentAction.component.name || pendingComponentAction.component.id}”？`
                  : `从电脑删除“${pendingComponentAction.component.name || pendingComponentAction.component.id}”？`}
            </h2>
            <p id="component-action-confirm-description">
              {pendingComponentAction.type === "remove-device"
                ? (
                  pendingComponentAction.wasActive
                    ? "这会卸载当前运行组件并让设备回到主屏。只有板端确认成功后才会清除“已启用”状态；正式本地组件库不受影响。"
                    : "这只会删除板端保存的这个组件包，当前正在运行的组件和本机组件库不受影响。"
                )
                : pendingComponentAction.installedOnDevice
                  ? "系统会先从当前设备删除组件；只有设备确认成功后，才会继续删除电脑中的组件源和自定义按键设置。"
                  : "这会删除电脑中的组件源和自定义按键设置；设备当前没有这个组件。"}
            </p>
            {pendingComponentAction.target && (
              <p className="component-action-confirm__target">
                <span>删除目标</span>
                <strong>{componentTargetLabel(pendingComponentAction.target)}</strong>
              </p>
            )}
            {componentActionError && (
              <p className="component-action-confirm__error" role="alert">
                {componentActionError}
              </p>
            )}
            <div>
              <button
                ref={componentActionCancelRef}
                className="btn-secondary"
                type="button"
                onClick={cancelComponentAction}
                disabled={componentActionPending}
              >
                取消
              </button>
              <button
                className="btn-ghost danger"
                type="button"
                onClick={confirmComponentAction}
                disabled={componentActionPending}
              >
                {pendingComponentAction.type === "remove-device"
                  ? <Unplug size={15} />
                  : <Trash2 size={15} />}
                {componentActionPending
                  ? pendingComponentAction.type === "remove-device"
                    ? "移除中…"
                    : pendingComponentAction.installedOnDevice
                      ? "双端删除中…"
                      : "删除中…"
                  : pendingComponentAction.type === "remove-device"
                    ? "确认从设备移除"
                    : pendingComponentAction.installedOnDevice
                      ? "确认从电脑和设备删除"
                      : "确认从电脑删除"}
              </button>
            </div>
          </section>
        </div>
      )}

      {createDrawerOpen && (
        <CreateComponentDrawer
          onClose={() => setCreateDrawerOpen(false)}
          handleInstallSkill={handleInstallSkill}
          skillInstalling={skillInstalling}
          skillInstallResult={skillInstallResult}
          clawpkgDragOver={clawpkgDragOver}
          setClawpkgDragOver={setClawpkgDragOver}
          handleClawpkgDrop={handleClawpkgDrop}
          handleClawpkgFilePick={handleClawpkgFilePick}
          clawpkgImporting={clawpkgImporting}
          clawpkgImportResult={clawpkgImportResult}
          componentLibraryPath={componentLibraryPath}
          componentLibraryCount={localComponents.length}
          componentLibraryLoading={componentLibraryLoading}
          componentLibraryMigration={componentLibraryMigration}
          refreshComponentLibrary={refreshComponentLibrary}
        />
      )}
      </div>
    </PageShell>
  );
}

function CreateNewCard({ onClick }) {
  return (
    <button
      type="button"
      className="candidate-card candidate-card--create"
      onClick={onClick}
    >
      <div className="candidate-card__preview candidate-card__preview--create">
        <Plus size={28} aria-hidden="true" />
      </div>
      <div className="candidate-card__body">
        <header className="candidate-card__head">
          <strong className="candidate-card__name">新建组件</strong>
        </header>
        <p className="candidate-card__goal">在当前 Agent 对话调用 petui，发布完成后会自动出现。</p>
      </div>
    </button>
  );
}

function CreateComponentDrawer({
  onClose,
  handleInstallSkill,
  skillInstalling,
  skillInstallResult,
  clawpkgDragOver,
  setClawpkgDragOver,
  handleClawpkgDrop,
  handleClawpkgFilePick,
  clawpkgImporting,
  clawpkgImportResult,
  componentLibraryPath,
  componentLibraryCount,
  componentLibraryLoading,
  componentLibraryMigration,
  refreshComponentLibrary,
}) {
  const [invocationCopied, setInvocationCopied] = useState(false);
  const invocationExample = "请使用 $petui，帮我生成一个桌搭子组件：<描述你的用途或玩法>";

  useEffect(() => {
    function onKey(event) {
      if (event.key === "Escape") onClose();
    }
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);

  async function copyInvocationExample() {
    try {
      await navigator.clipboard.writeText(invocationExample);
      setInvocationCopied(true);
      window.setTimeout(() => setInvocationCopied(false), 1800);
    } catch (error) {
      console.warn("[ComponentCenter] copy petui invocation failed", error);
    }
  }

  return (
    <div
      className="component-center-drawer-backdrop"
      role="presentation"
      onClick={onClose}
    >
      <aside
        className="component-center-drawer"
        role="dialog"
        aria-modal="true"
        aria-label="创建组件"
        onClick={(event) => event.stopPropagation()}
      >
        <header className="component-center-drawer__head">
          <div>
            <h2>创建组件</h2>
            <p>安装 petui 后，在你正在使用的 Agent 对话中生成；校验通过的组件会发布到正式本地组件库。</p>
          </div>
          <button
            type="button"
            className="icon-btn"
            onClick={onClose}
            aria-label="关闭抽屉"
          >
            <X size={16} />
          </button>
        </header>

        <article className="component-tool-card component-tool-card--skill">
          <header>
            <span className="component-tool-eyebrow">STEP 1 · 装 Skill</span>
            <h3>安装 petui</h3>
            <p>安装到检测到的 Codex、Claude、OpenClaw、MiMoCode 等兼容目录。旧版组件生成 Skill 会被替换。</p>
          </header>
          <button
            className="btn-primary component-skill-install-button"
            type="button"
            onClick={handleInstallSkill}
            disabled={skillInstalling}
          >
            <PackageCheck size={15} />
            {skillInstalling ? "正在安装…" : "一键安装 Skill"}
          </button>
          {skillInstallResult && (
            <div className="component-skill-install-result">
              {skillInstallResult.installed.length > 0 && (
                <>
                  <p className="component-tool-result__title">已安装到 {skillInstallResult.installed.length} 个 coding agent</p>
                  <ul>
                    {skillInstallResult.installed.map((entry) => (
                      <li key={entry.agent}>
                        <strong>{entry.agent}</strong>
                        <span>{entry.fileCount} 文件{entry.overwrote ? " · 覆盖更新" : ""}</span>
                        <code>{entry.targetPath}</code>
                      </li>
                    ))}
                  </ul>
                </>
              )}
              {skillInstallResult.skipped.length > 0 && (
                <details className="component-skill-install-skipped">
                  <summary>跳过了 {skillInstallResult.skipped.length} 个未检测到的 agent</summary>
                  <ul>
                    {skillInstallResult.skipped.map((entry) => (
                      <li key={entry.agent}>
                        <strong>{entry.agent}</strong>
                        <span>{entry.reason}</span>
                      </li>
                    ))}
                  </ul>
                </details>
              )}
            </div>
          )}
        </article>

        <article className="component-tool-card component-tool-card--generate">
          <header>
            <span className="component-tool-eyebrow">STEP 2 · 在 Agent 中生成</span>
            <h3>开打Agent（如codex）</h3>
            <p>复制下方示例文案到Agent界面，与Agent对话生成你想要在设备上使用的小组件或小游戏</p>
          </header>
          <code className="component-petui-invocation">{invocationExample}</code>
          <button className="btn-secondary" type="button" onClick={copyInvocationExample}>
            <Clipboard size={15} />
            {invocationCopied ? "已复制" : "复制调用示例"}
          </button>
        </article>

        <article className="component-tool-card component-tool-card--clawpkg">
          <header>
            <span className="component-tool-eyebrow">STEP 3 · 正式本地组件</span>
            <h3>发布后自动进入组件库</h3>
            <p>petui 发布成功后会自动刷新。也可以手动导入已有的 <code>.clawpkg</code> 或 zip；导入会先校验并发布，不会直接下发到设备。</p>
          </header>
          <div className="component-library-location" role="status">
            <span>正式本地组件 {componentLibraryCount} 个</span>
            <code>{componentLibraryPath || "~/.claw-pet/components/library"}</code>
            {componentLibraryMigration?.migratedCount > 0 && (
              <small>已从旧目录保留并迁移 {componentLibraryMigration.migratedCount} 个组件</small>
            )}
          </div>
          <button
            type="button"
            className="btn-secondary"
            onClick={refreshComponentLibrary}
            disabled={componentLibraryLoading}
          >
            <RefreshCw size={15} />
            {componentLibraryLoading ? "刷新中…" : "刷新正式组件库"}
          </button>
          <div
            className={`component-clawpkg-dropzone ${clawpkgDragOver ? "is-dragover" : ""}`}
            onDragOver={(e) => { e.preventDefault(); setClawpkgDragOver(true); }}
            onDragLeave={() => setClawpkgDragOver(false)}
            onDrop={handleClawpkgDrop}
          >
            <Clipboard size={20} />
            <span>{clawpkgImporting ? "正在校验并发布…" : "拖拽 .clawpkg 目录 / zip 到这里"}</span>
          </div>
          <button
            type="button"
            className="btn-secondary component-clawpkg-pick-button component-clawpkg-fallback-button"
            onClick={handleClawpkgFilePick}
            disabled={clawpkgImporting}
          >
            <Clipboard size={15} />
            选择并导入正式组件库
          </button>
          {clawpkgImportResult && (
            <p className="component-tool-result__inline">
              已发布: <strong>{clawpkgImportResult.manifest.name}</strong>，请在预览页核对按钮配置
            </p>
          )}
        </article>
      </aside>
    </div>
  );
}
