/**
 * [Input] Consume component-center package fixtures from `pc/src/fixtures.js`[Pos].
 * [Output] Component manager in the shared Pet Manager visual system: one type-filtered
 *          game/tool library whose cards reflect live board truth and perform immediate
 *          per-component sync/removal. The manager chrome stays product-native while every widget
 *          preview and generated package uses the bounded clean/pixel visual contract, including
 *          modern scene silhouettes and safe animated sprite sheets. Formal local
 *          gameplay-only buttons.json is editable across screen/SW1-SW3/four-direction
 *          joystick inputs, conflict-checked against the board-authoritative global exit
 *          gesture, persisted by component/action, and stored inside the installed P4 component.
 *          Live inventory, not localStorage, is authoritative for what is on the board. The
 *          short-lived session cache is only a stale placeholder; entry/reconnect and visible
 *          polling revalidate the board. Old requests cannot refresh a new connection's cache.
 *          Formal local deletion is a device-first transaction when the package is installed,
 *          so a failed board ACK never destroys the only local source. Generated formal-local
 *          packages lead newest-first; builtins follow in product order with stocks first.
 *          Same-id local/builtin sources share one card with an explicit source selector;
 *          newer valid resources are preferred without deleting custom revisions.
 *          Cards open a common confirmation flow. Button drafts persist only after ACK;
 *          PC deletion archives all local revisions, while device deletion remains explicit.
 *          The page subtitle explains that the top-right creation action installs the shared
 *          Skill into an Agent for iterative generation and automatic library discovery;
 *          type filters stay on a compact row below those title actions. Stock management
 *          lives only in the stock component detail modal.
 *          A first-visit quick-start modal explains browse/create and direct card sync, and
 *          remains reopenable from the page help action. Component generation stays
 *          in the user's current Agent conversation through the editable frog-pet example:
 *          petui validates and atomically publishes
 *          to the formal local library, which this page watches and refreshes without launching CLIs
 *          or exposing a separate manual package-import flow.
 * [Pos] component node in pc/src
 * [Sync] If this file changes, update this header and `pc/src/.folder.md`.
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
import { listen } from "@tauri-apps/api/event";
import { watch } from "@tauri-apps/plugin-fs";
import { useExitProtection } from "./lib/exit-protection.js";
import { componentRequiresPc } from "./component-center/catalog-state.js";
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
import CommunityCenter from "./component-center/CommunityCenter.jsx";
import RemovedComponents from './component-center/RemovedComponents.jsx';
import WorkflowConfirm from './component-center/WorkflowConfirm.jsx';
import {useDialogFocus} from './component-center/use-dialog-focus.js';
import './component-center/center-workflow.css';
import {devicePackageState, readCenterPreference, saveCenterPreference} from './component-center/catalog-state.js';
import { isRoutedWidgetBinding } from "./component-center/binding-labels.js";
import { isComponentVisible, mergeComponentCatalog, sortComponentsByCreatedAt } from "./component-center/library-order.js";
import {
  COMPONENT_CONTROL_OPTIONS,
  componentInputEventSlots,
  componentControlOptionAllowed,
  defaultControlLabelForBinding,
  globalExitControlLabel,
  optionForControlLabel,
  resolveGlobalExitEvents,
} from "./component-center/button-config.js";

const COMPONENT_BUTTON_OVERRIDES_STORAGE_KEY = "pet-manager:component-button-overrides:v1";
// Formal-library source revisions that have been promoted into the builtin
// catalog. Hide only the exact promoted revision; later user revisions keep
// their normal newest-first position ahead of builtins.
const PROMOTED_BUILTIN_SOURCE_HASHES = new Set([
  "75b1737728db27be",
  "b7cd756fc47c42b3",
  "e445d1af162d2470",
  "e725dd72d4a5bea1",
  "b8d4bb1230e6f3d7",
]);
// View remounts reuse the last successful snapshot; refresh never clears cards.
let lastLibrarySnapshot = null;
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
  if(component?.pcExtension?.discovery && (Number(usb?.capabilities?.widgetMediaQueueMax||20)<128 || usb?.capabilities?.widgetMediaLifecycle!=="p4-media-lifecycle-v1")) return "自动电台列表需要升级设备固件（媒体队列与进入自动播放能力）";
  if ((component?.inputPolicy || component?.appRuntime?.input) && usb?.capabilities?.widgetInput !== "p4-input-v1") return "需要新版触屏、按键与摇杆交互能力，请先升级设备固件";
  if(component?.appRuntime?.lifecycle&&usb?.capabilities?.widgetLifecycle!=="p4-lifecycle-v1")return "需要游戏暂停与成绩存档能力，请先升级设备固件";
  const surface=component?.mediaSurface||component?.appRuntime?.surface;
  if (surface?.fonts?.some(f=>f.cell) && usb?.capabilities?.widgetGlyphCells!=="p4-glyph-cells-v1") return "需要高清字形格能力，请先升级设备固件";
  if(surface?.input&&usb?.capabilities?.widgetMediaTouch!=="p4-media-touch-v1")return "需要媒体触屏控制能力，请先升级设备固件";
  if(surface?.images?.some(im=>im.encoding!=="jpeg"&&im.frame_width>240)&&usb?.capabilities?.widgetSurfaceAtlas!=="p4-wide-atlas-v1")return "需要高清分格素材能力，请先升级设备固件";
  if(component?.appRuntime?.game?.vertical_speed_percent!==undefined && usb?.capabilities?.widgetFlappyTuning!=="p4-flappy-tuning-v1") return "需要飞鸟速度参数能力，请先升级设备固件";
  if((component?.mediaSurface?.game_view || component?.appRuntime?.surface?.game_view) && usb?.capabilities?.widgetGameSurface!=="p4-game-surface-v1") return "需要精细游戏画面能力，请先升级设备固件";
  if((component?.mediaSurface?.data_view || component?.appRuntime?.surface?.data_view) && usb?.capabilities?.widgetDataSurface!=="p4-data-surface-v1") return "需要精细数据画面能力，请先升级设备固件";
  if((component?.mediaSurface?.scene_view || component?.appRuntime?.surface?.scene_view) && usb?.capabilities?.widgetSceneSurface!=="p4-scene-surface-v1") return "需要精细场景画面能力，请先升级设备固件";
  if(component?.mediaSurface?.engine==="p4-app-surface-v1" && usb?.capabilities?.widgetAppSurface!=="p4-app-surface-v1") return "需要通用应用界面能力，请先升级设备固件";
  if(component?.mediaSurface?.graphics && usb?.capabilities?.widgetAppGraphics!=="p4-app-graphics-v1") return "需要高清背景、图标和精细排版能力，请先升级设备固件";
  if(component?.mediaSurface?.typography && usb?.capabilities?.widgetAppTypography!=="p4-app-type-v1") return "需要组件字形和动态按键提示能力，请先升级设备固件";
  if(component?.mediaSurface?.interaction && usb?.capabilities?.widgetAppInteraction!=="p4-app-interaction-v1") return "需要组件分格素材与条件显示能力，请先升级设备固件";
  if(component?.appRuntime?.persist && usb?.capabilities?.widgetState!=="p4-app-state-v1") return "需要组件存档能力，请先升级设备固件";
  if(component?.appRuntime?.host && usb?.capabilities?.widgetAppHost!=="p4-app-host-v1") return "需要组件宿主服务能力，请先升级设备固件和 PC 客户端";
  if(component?.mediaSurface?.images?.length && usb?.capabilities?.widgetSurfaceImages!=="p4-surface-images-v1") return "需要图片与旋转能力，请先升级设备固件";
  if(component?.scene?.engine === "p4-touch-slice-v1" && (component.scene.revision||1)>(usb?.capabilities?.widgetSliceRevision||1)) {
    return "这个游戏版本需要新版切果能力，请先升级设备固件。";
  }
  if(component?.scene?.engine === "p4-touch-slice-v1" && (usb?.capabilities?.widgetPointer!=="p4-pointer-v1" || usb?.capabilities?.touchInput?.ready!==true)) {
    return "这个游戏需要连续触控，请更新固件并确认触屏就绪。";
  }
  if (component?.instrumentSource && usb?.capabilities?.widgetInstrument !== "p4-instrument-v1") {
    return "这个组件需要本地木鱼音效能力，请先升级设备固件。";
  }
  // Capability requirements depend on declared actions, not per-user key remapping.
  // This module-level function must not call ComponentCenter's render-local helpers.
  const bindings = Array.isArray(component?.defaultBindings) ? component.defaultBindings : [];
  if (bindings.some((binding) => binding?.action === "media.lyrics")
      && usb?.capabilities?.widgetLyrics !== "p4-lrc-v1") {
    return "这个组件的歌词模式需要更新固件，请先升级设备固件。";
  }
  if (component?.pcExtension && usb?.capabilities?.widgetMediaSurface !== "p4-media-surface-v1") {
    return "当前固件不支持此媒体组件，请先升级固件";
  }
  if ((component?.mediaSource || component?.id === "music-player") && usb?.capabilities?.widgetMedia !== "p4-media-v1") {
    return "这个组件需要音乐播放能力，请先升级设备固件。";
  }
  if (component?.dataSource && usb?.capabilities?.widgetData !== "p4-data-list-v1") {
    return "这个组件需要实时数据列表能力，请先升级设备固件。";
  }
  const requiredRuntime = String(component?.runtimeEngine || "");
  const requiredScene = String(component?.sceneEngine || "");
  const preset = String(component?.gamePreset || component?.gameType || "");
  if (!requiredRuntime && !requiredScene && !preset) return "";
  const runtime = String(usb?.capabilities?.widgetRuntime || "");
  const scene = String(usb?.capabilities?.widgetScene || "");
  const runtimes = Array.isArray(usb?.capabilities?.widgetRuntimes)
    ? usb.capabilities.widgetRuntimes.map(String)
    : runtime
      ? [runtime]
      : [];
  const scenes = Array.isArray(usb?.capabilities?.widgetScenes)
    ? usb.capabilities.widgetScenes.map(String)
    : scene
      ? [scene]
      : [];
  const presets = Array.isArray(usb?.capabilities?.widgetGamePresets)
    ? usb.capabilities.widgetGamePresets
    : Array.isArray(usb?.capabilities?.widgetGames)
      ? usb.capabilities.widgetGames
    : [];
  if (requiredRuntime && !runtimes.includes(requiredRuntime)) {
    return `这个组件需要 P4 通用运行时（${requiredRuntime}），请先升级设备固件。`;
  }
  if (requiredScene && !scenes.includes(requiredScene)) {
    return `这个组件需要 P4 场景能力（${requiredScene}），请先升级设备固件。`;
  }
  if (preset && !presets.includes(preset)) {
    return `这个旧版游戏预设需要设备能力（${preset}），请先升级设备固件。`;
  }
  if (
    preset &&
    !runtimes.some((value) =>
      ["p4-bounded-v2", "p4-bounded-runtime-v3", "p4-bounded-runtime-v4"].includes(value),
    )
  ) {
    return `这个旧版游戏预设需要 P4 组件运行时，请先升级设备固件。`;
  }
  return "";
}

function localInstallBlockedReason(component) {
  if (component?.valid !== false) return "";
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
  const cached = targetKey ? SESSION_DEVICE_INVENTORY_CACHE.get(targetKey) : null;
  return cached && Date.now() - cached.checkedAt < 15000 ? cached : null;
}

function writeSessionDeviceInventory(target, inventory) {
  const targetKey = activeComponentTargetKey(target);
  if (targetKey && inventory) {
    SESSION_DEVICE_INVENTORY_CACHE.set(targetKey, {...inventory,checkedAt:Date.now()});
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
  }).then((result) => {
    const inventory=normalizeDeviceInventory(result);
    return SESSION_DEVICE_INVENTORY_REQUESTS.get(targetKey)===request?writeSessionDeviceInventory(target,inventory):inventory;
  }).catch((error) => {
    console.warn("[ComponentCenter] list_device_widgets failed", error);
    const inventory=inventoryFailure(error,target);
    return SESSION_DEVICE_INVENTORY_REQUESTS.get(targetKey)===request?writeSessionDeviceInventory(target,inventory):inventory;
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

export default function ComponentCenter({ active = true }) {
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
  const [trashOpen,setTrashOpen]=useState(false);
  const [downgradeComponent,setDowngradeComponent]=useState(null);
  const [previewComponent, setPreviewComponent] = useState(null);
  const [activeComponentRecord, setActiveComponentRecord] = useState(deviceCurrentComponent);
  const [bindingOverrides, setBindingOverrides] = useState(loadComponentButtonOverrides);
  const [savedBindingOverrides, setSavedBindingOverrides] = useState(loadComponentButtonOverrides);
  const syncInFlight = useRef(false);
  const transferInFlight = useRef(false);
  const installEpoch=useRef(0);
  useEffect(()=>()=>{installEpoch.current++;},[]);
  const targetRef = useRef(liveInventoryTarget);
  targetRef.current = liveInventoryTarget;
  const [skillInstalling, setSkillInstalling] = useState(false);
  const [skillInstallResult, setSkillInstallResult] = useState(null);
  const [localComponents, setLocalComponents] = useState(()=>lastLibrarySnapshot?.components||[]);
  const [componentLibraryPath, setComponentLibraryPath] = useState(()=>lastLibrarySnapshot?.libraryPath||"");
  const [libraryReady,setLibraryReady] = useState(()=>lastLibrarySnapshot!==null);
  const [componentLibraryLoading, setComponentLibraryLoading] = useState(false);
  const [componentLibraryError, setComponentLibraryError] = useState("");
  const [pendingComponentAction, setPendingComponentAction] = useState(null);
  const [componentActionPending, setComponentActionPending] = useState(false);
  const [componentActionError, setComponentActionError] = useState("");
  const [deviceInventory, setDeviceInventory] = useState(
    () => readSessionDeviceInventory(liveInventoryTarget) || EMPTY_DEVICE_INVENTORY,
  );
  const [inventoryLoading, setInventoryLoading] = useState(false);
  const [libraryKind, setLibraryKind] = useState(()=>readCenterPreference('kind','all'));
  const [libraryQuery,setLibraryQuery] = useState(()=>readCenterPreference('query',''));
  const [libraryStatus,setLibraryStatus] = useState(()=>readCenterPreference('status','all'));
  const [centerTab,setCenterTab] = useState('local');
  useEffect(()=>{saveCenterPreference('kind',libraryKind);saveCenterPreference('query',libraryQuery);saveCenterPreference('status',libraryStatus);},[libraryKind,libraryQuery,libraryStatus]);
  function openComponent(component) {if(!isComponentVisible(component))return;setBindingOverrides(savedBindingOverrides);setPreviewComponent(component);}
  function closeComponent() {setBindingOverrides(savedBindingOverrides);setPreviewComponent(null);}
  const [globalExitBinding, setGlobalExitBinding] = useState({
    boardDeviceId: "",
    events: [],
    status: "idle",
  });
  const globalExitEvents = globalExitBinding.events;
  const libraryRefreshRequestRef = useRef(0);
  const inventoryRefreshRequestRef = useRef(0);
  const componentActionDialogRef = useRef(null);
  const componentActionCancelRef = useRef(null);
  const componentActionReturnFocusRef = useRef(null);
  /* USB OTA install flow modal: idle | checking-usb | waiting-usb | installing | success | error */
  const [otaPhase, setOtaPhase] = useState("idle");
  useExitProtection(otaPhase === "installing" || skillInstalling || componentActionPending ? "组件安装或更新尚未完成" : "");
  const [otaProgress, setOtaProgress] = useState(null);
  const [communityRefreshKey, setCommunityRefreshKey] = useState(0);
  useEffect(() => {
    let unlisten, disposed=false;
    listen("component-install-progress",({payload})=>setOtaProgress(payload)).then((off)=>{if(disposed)off();else unlisten=off;}).catch(()=>{});
    return ()=>{disposed=true;unlisten?.();};
  },[]);
  const [otaPendingPath, setOtaPendingPath] = useState(null);
  const [otaPendingOptions, setOtaPendingOptions] = useState({});
  const [otaTargetName, setOtaTargetName] = useState("");
  const [otaError, setOtaError] = useState(null);
  const [otaResult, setOtaResult] = useState(null);

  useEffect(() => {
    setActiveComponentRecord(deviceCurrentComponent);
  }, [deviceCurrentComponent]);

  useEffect(() => {
    const boardDeviceId = String(usb.boardDeviceId || "").trim();
    if (!usb.connected || !boardDeviceId || String(usb.runtime || "").toLowerCase() !== "esp-p4") {
      setGlobalExitBinding({ boardDeviceId: "", events: [], status: "idle" });
      return undefined;
    }
    let cancelled = false;
    setGlobalExitBinding({ boardDeviceId, events: [], status: "loading" });
    invoke("usb_get_button_config", { expectedBoardDeviceId: boardDeviceId })
      .then((response) => {
        if (!cancelled) {
          setGlobalExitBinding({
            boardDeviceId,
            events: resolveGlobalExitEvents(response),
            status: "ready",
          });
        }
      })
      .catch((error) => {
        console.warn("[ComponentCenter] global exit binding query failed", error);
        if (!cancelled) setGlobalExitBinding({ boardDeviceId, events: [], status: "error" });
      });
    return () => {
      cancelled = true;
    };
  }, [usb.boardDeviceId, usb.connected, usb.runtime]);

  const refreshComponentLibrary = useCallback(async () => {
    const requestId = libraryRefreshRequestRef.current + 1;
    libraryRefreshRequestRef.current = requestId;
    setComponentLibraryLoading(true);
    try {
      const snapshot = await invoke("list_component_library");
      if (requestId === libraryRefreshRequestRef.current) {
        lastLibrarySnapshot={components:Array.isArray(snapshot?.components)?snapshot.components:[],libraryPath:String(snapshot?.libraryPath||"")};
        setLocalComponents(Array.isArray(snapshot?.components) ? snapshot.components : []);
        setComponentLibraryPath(String(snapshot?.libraryPath || ""));
        setComponentLibraryError("");
      }
    } catch (err) {
      console.warn("[ComponentCenter] list_component_library failed", err);
      if(requestId===libraryRefreshRequestRef.current)setComponentLibraryError(String(err));
    } finally {
      if (requestId === libraryRefreshRequestRef.current) {setComponentLibraryLoading(false);setLibraryReady(true);}
    }
  }, []);

  /* Load immediately and keep a low-frequency fallback for platforms where
     native file notifications are unavailable or temporarily interrupted. */
  useEffect(() => {
    if (!active) return;
    refreshComponentLibrary();
    const interval = setInterval(()=>{if(document.visibilityState!=='hidden')refreshComponentLibrary();}, 30000);
    return () => clearInterval(interval);
  }, [active, refreshComponentLibrary]);

  useEffect(() => {
    if (!active || !componentLibraryPath) return undefined;
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
  }, [active, componentLibraryPath, refreshComponentLibrary]);

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
    if (!active) return;
    const cachedInventory = readSessionDeviceInventory(liveInventoryTarget);
    if (cachedInventory) {
      inventoryRefreshRequestRef.current += 1;
      setDeviceInventory({...cachedInventory,freshness:'stale'});
    } else {
      setDeviceInventory(EMPTY_DEVICE_INVENTORY);
    }
    refreshDeviceInventory();
    return ()=>{
      inventoryRefreshRequestRef.current+=1;
      SESSION_DEVICE_INVENTORY_REQUESTS.delete(activeComponentTargetKey(liveInventoryTarget));
    };
  }, [active, liveInventoryTarget, refreshDeviceInventory]);

  useEffect(()=>{
    if(!active || !liveInventoryTarget)return;
    const timer=setInterval(()=>{if(!syncInFlight.current&&document.visibilityState!=='hidden')refreshDeviceInventory({silent:true});},15000);
    return()=>clearInterval(timer);
  },[active,liveInventoryTarget,refreshDeviceInventory]);

  useEffect(() => {
    if (typeof window === "undefined" || !window.localStorage) return;
    try {
      window.localStorage.setItem(
        COMPONENT_BUTTON_OVERRIDES_STORAGE_KEY,
        JSON.stringify(savedBindingOverrides),
      );
    } catch (err) {
      console.warn("[ComponentCenter] failed to persist component button overrides", err);
    }
  }, [savedBindingOverrides]);

  /** Build the shared preview/install shape from a formally published entry. */
  function buildLibraryComponent(entry) {
    return {
      id: entry.id,
      version: entry.version || "",
      name: entry.name || entry.id,
      goal: buildLibraryGoal(entry),
      dashboard: entry.dashboard || {},
      gameType: entry.gameType || "",
      runtimeEngine: entry.runtimeEngine || "",
      dataSource: entry.dataSource || "",
      mediaSource: entry.mediaSource || "",
      pcExtension: entry.pcExtension || null,
      mediaSurface: entry.mediaSurface || null,
      appRuntime: entry.appRuntime || null,
      inputPolicy: entry.inputPolicy || null,
      pcApplication: entry.pcApplication || null,
      author: entry.author || "",
      instrumentSource: entry.instrumentSource || "",
      sceneEngine: entry.sceneEngine || "",
      gamePreset: entry.gamePreset || "",
      scene: entry.scene || null,
      sceneSprites: Array.isArray(entry.sceneSprites) ? entry.sceneSprites : [],
      kind: resolveComponentKind(entry.kind, entry.gameType),
      defaultBindings: Array.isArray(entry.buttons) ? entry.buttons.filter(isRoutedWidgetBinding) : [],
      screens: [{ name: "负一屏", purpose: "正式本地组件自带独立按钮功能绑定", regions: [] }],
      status: "library",
      path: entry.path,
      libraryPath: entry.path,
      versionHash: entry.versionHash || "",
      isBundledCopy: entry.isBundledCopy===true,
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
    const record = isComponentVisible(activeComponentRecord) ? activeComponentRecord : null;
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

  /** Recently installed user packages stay first; remaining builtins use the
   *  curated default order, without losing custom revisions or source choices. */
  const catalogItems = useMemo(() => {
    const builtins = BUILTIN_COMPONENT_CENTER.components.map((item) => ({
      ...item,
      kind: resolveComponentKind(item.kind, item.gameType),
      isLocal: false,
    }));
    const publishedItems = sortComponentsByCreatedAt(
      localComponents
        .filter((entry) => !PROMOTED_BUILTIN_SOURCE_HASHES.has(entry.versionHash))
        .map((entry) => buildLibraryComponent(entry)),
    );
    return mergeComponentCatalog(publishedItems, builtins);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [localComponents]);

  /** Packages reported by the board but absent from this PC are folded into the
   *  same grid. They remain removable, but cannot be installed or edited locally. */
  const libraryItems = useMemo(() => {
    if (deviceInventory.freshness !== "live") return catalogItems;
    const catalogIds = new Set(catalogItems.map((item) => item.id));
    const deviceOnlyItems = deviceInventory.items
      .filter((item) => isComponentVisible(item) && !catalogIds.has(item.id))
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
      const state=devicePackageState(item,deviceInventory);
      const query=String(libraryQuery).trim().toLowerCase();
      const matchesStatus=libraryStatus==='all'||(libraryStatus==='installed'&&installedIds.has(item.id)&&deviceInventory.freshness==='live')||(libraryStatus==='update'&&state.key==='update')||(libraryStatus==='pc'&&componentRequiresPc(item))||(libraryStatus==='offline'&&!componentRequiresPc(item));
      return (libraryKind === "all" || kind === libraryKind)&&matchesStatus&&(!query||`${item.name} ${item.id} ${item.author||''}`.toLowerCase().includes(query));
    }),
    [libraryItems, libraryKind,libraryQuery,libraryStatus,deviceInventory,installedIds],
  );

  const librarySectionRef = useRef(null);
  const previewIsInstalled = Boolean(
    previewComponent
    && deviceInventory.freshness === "live"
    && installedIds.has(previewComponent.id),
  );
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
    setCenterTab('local');
    window.setTimeout(
      () => {
        const section=librarySectionRef.current;
        if(!section)return;
        const reducedMotion=window.matchMedia?.('(prefers-reduced-motion: reduce)')?.matches;
        section.scrollIntoView({behavior:reducedMotion?'auto':'smooth',block:'start'});
        section.querySelector('.component-library-filters button[aria-pressed="true"]')?.focus({preventScroll:true});
      },
      0,
    );
  }

  async function syncSelectedComponent(component, allowDowngrade=false) {
    if (!component?.id || component.isDeviceOnly || syncInFlight.current) return;
    const conflict=bindingConflictForComponent(component);
    if(conflict){push({tone:'warning',title:'请先调整按钮设置',message:conflict});return;}
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
    if(!allowDowngrade&&devicePackageState(component,deviceInventory).key==='newer'){setDowngradeComponent(component);return;}
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
    syncInFlight.current=true;
    try {await installSelectedComponent(component);} catch(error){push({tone:'error',title:'同步未开始',message:String(error)});} finally {syncInFlight.current=false;}
  }

  async function installSelectedComponent(component = previewComponent) {
    if (!component || !isComponentVisible(component)) return false;
    setPreviewComponent(null);

    /* Formal local components ship their own buttons.json + dashboard. Builtins
       resolve through the Tauri backend so packaged apps use bundled resources
       instead of assuming ~/.openclaw exists on every user's machine. */
    const localPath = component.libraryPath || component.draftPath;
    const options={component,bindingSnapshot:{...bindingOverrides},bindingMap:buildBindingOverridesForInstall(component),target:liveInventoryTarget};
    if (localPath) {
      return installClawpkgFromPath(localPath, {
        targetName: component.name,
        skipFooterOverride: true,
        component,
        ...options,
      });
    }
    return installBuiltinToDevice(component,options);
  }

  function markComponentInstalled(component, target) {
    if (!component || !target) return;
    const record = writeActiveComponentForTarget(component, target);
    setActiveComponentRecord(record);
    window.dispatchEvent(new Event("storage"));
  }

  function requestComponentDelete(component, installedOnDevice = false) {
    if (!component?.id) return;
    if (installedOnDevice && (deviceInventory.freshness !== "live" || !liveInventoryTarget)) {
      push({
        tone: "warning",
        title: "请先连接并读取设备清单",
        message: "需要先确认当前设备，才能选择“仅从设备删除”或执行双端删除。",
      });
      return;
    }
    const inventoryItem = installedOnDevice
      ? deviceInventory.items.find((item) => item.id === component.id)
      : null;
    componentActionReturnFocusRef.current = document.activeElement;
    setPendingComponentAction({
      type: "delete-options",
      component,
      isLocal: component.isLocal === true,
      installedOnDevice,
      target: installedOnDevice ? liveInventoryTarget : null,
      wasActive: inventoryItem?.active === true,
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
    setSavedBindingOverrides(current=>Object.fromEntries(Object.entries(current).filter(([key])=>!key.startsWith(keyPrefix))));
  }

  async function confirmComponentAction(mode) {
    if (!pendingComponentAction?.component || componentActionPending) return;
    const {
      component,
      target,
      wasActive = false,
      installedOnDevice = false,
      isLocal = false,
    } = pendingComponentAction;
    const deleteLibrary = mode === "delete-library";
    const removesDevice = mode === "device-only" || (deleteLibrary && installedOnDevice);
    if (!removesDevice && !deleteLibrary) return;
    if (deleteLibrary && !isLocal) return;
    setComponentActionPending(true);
    setComponentActionError("");
    let deviceRemoved=false;
    try {
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
        deviceRemoved=true;
        setDeviceInventory((current) => writeSessionDeviceInventory(target, {
          ...current,
          activeWidgetId: wasActive ? "" : current.activeWidgetId,
          items: current.items.filter((item) => item.id !== component.id),
        }));
      }

      if (deleteLibrary) {
        // Device removal happens first. A failed board ACK leaves the local
        // package intact, so users never lose the only source by accident.
        const targetPath = component.libraryPath || component.path;
        if (!targetPath) throw new Error("找不到要删除的正式本地组件路径");
        await invoke("delete_component_from_library", { input: { path: targetPath } });
        setLocalComponents((current) => current.filter(
          (entry) => normalizeLocalPath(entry.path) !== normalizeLocalPath(targetPath),
        ));
        // Keep mappings with the recoverable source; restore reuses its original path.
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
            ? `设备已移除，本机源已放入回收站 · ${component.name || component.id}`
            : `已放入组件回收站 · ${component.name || component.id}`,
          message: deviceResult?.warning || '可在“我的组件 → 回收站”恢复；本机用户记录保留。',
        });
      } else {
        setPreviewComponent(null);
        push({
          tone: deviceResult?.warning ? "warning" : "success",
          title: `已从设备删除 · ${component.name || component.id}`,
          message: deviceResult?.warning || undefined,
        });
      }
      if (removesDevice) await refreshDeviceInventory({ silent: true });
      setPendingComponentAction(null);
      restoreComponentActionFocus();
    } catch (err) {
      if(deviceRemoved){setPendingComponentAction(old=>old?{...old,installedOnDevice:false,wasActive:false}:old);await refreshDeviceInventory({silent:true});}
      setComponentActionError(`${deviceRemoved?'设备已移除；本机源未移入回收站，可重试本机操作。 ':''}${componentActionErrorMessage(err)}`);
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
    const boardDeviceId = String(usb.boardDeviceId || "").trim();
    if (
      usb.connected
      && String(usb.runtime || "").toLowerCase() === "esp-p4"
      && boardDeviceId
      && (globalExitBinding.boardDeviceId !== boardDeviceId || globalExitBinding.status === "loading")
    ) {
      return "正在读取设备的全局按键配置，请稍候";
    }
    if (
      usb.connected
      && String(usb.runtime || "").toLowerCase() === "esp-p4"
      && boardDeviceId
      && globalExitBinding.boardDeviceId === boardDeviceId
      && globalExitBinding.status === "error"
    ) {
      return "读取设备的全局按键配置失败，请重新连接设备后再试";
    }
    const seen = new Map();
    for (const binding of resolveComponentBindings(component)) {
      if (binding.event.startsWith("screen.") && !deviceTouchReady(usb)) {
        return `${binding.label} 使用了屏幕手势，但当前设备未报告触屏可用，请改为未被全局退出占用的按键或摇杆`;
      }
      for (const eventSlot of componentInputEventSlots(binding.event)) {
        if (globalExitEvents.includes(eventSlot)) {
          return `${binding.label} 占用了当前全局退出键，请改用其他按键`;
        }
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
    return component.defaultBindings.filter(isRoutedWidgetBinding).reduce((overrides, binding) => {
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
      .filter((option) => componentControlOptionAllowed(option, binding, component))
      .filter((option) => (
        !option.event.startsWith("screen.")
        || deviceTouchReady(usb)
        || option.event === currentEvent
      ))
      .map((option) => ({
        ...option,
        disabled: (
          componentInputEventSlots(option.event).some((event) => globalExitEvents.includes(event))
          || (option.event.startsWith("screen.") && !deviceTouchReady(usb))
          || (
            option.event !== currentEvent
            && componentInputEventSlots(option.event).some((event) => usedEvents.has(event))
          )
        ),
        disabledReason: componentInputEventSlots(option.event).some((event) => globalExitEvents.includes(event))
          ? "全局退出"
          : "已占用",
      }));
  }

  function resetBindings(component) {
    setBindingOverrides((current) => {
      const next = { ...current };
      (component?.defaultBindings || []).forEach((binding) => {
        delete next[bindingKey(component, binding.action)];
        delete next[`${component.id}:${binding.action}`];
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
    options={...options,epoch:++installEpoch.current};
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
    if(options.epoch!==installEpoch.current)return false;
    if (!status?.connected && !deviceConnected) {
      setOtaPhase("waiting-usb");
      return false;
    }
    return performOtaInstall(componentId, clawpkgPath, options);
  }

  async function performOtaInstall(componentId, clawpkgPath, options = {}) {
    if(options.epoch!==undefined&&options.epoch!==installEpoch.current)return false;
    if(transferInFlight.current)return false;
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
    transferInFlight.current=true;
    try {
      if(options.target&&activeComponentTargetKey(options.target)!==activeComponentTargetKey(targetRef.current))throw new Error('目标设备已变化，请关闭后重新确认同步目标。');
      let result;
      let installTarget;
      let liveStatus = null;
      try {
        liveStatus = await invoke("usb_get_status");
      } catch (err) {
        // The install path below reports the actionable transport error.
      }
      const liveUsbConnected = Boolean(liveStatus?.connected);
      if(options.epoch!==undefined&&options.epoch!==installEpoch.current)return false;
      const useSsh = !options.forceUsb && !liveUsbConnected && sshHost.length > 0;
      attemptedSsh = useSsh;
      if (useSsh) {
        installTarget = { transport: "ssh", sshHost };
        if(options.target&&activeComponentTargetKey(options.target)!==activeComponentTargetKey(installTarget))throw new Error('连接方式或目标已变化，请重新确认同步目标。');
        result = await invoke("install_clawpkg_over_ssh", {
          input: {
            clawpkgPath,
            sshHost,
            bindingOverrides: options.bindingMap || buildBindingOverridesForInstall(component),
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
        if(options.target&&activeComponentTargetKey(options.target)!==activeComponentTargetKey(installTarget))throw new Error('目标设备已变化，请重新确认同步目标。');
        setOtaProgress(null);
        const footerOverride = options.skipFooterOverride ? "" : buildBindingsFooter(component);
        result = await invoke("install_clawpkg_over_usb", {
          input: {
            clawpkgPath,
            expectedBoardDeviceId: boardDeviceId,
            footerOverride,
            bindingOverrides: options.bindingMap || buildBindingOverridesForInstall(component),
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
      if(options.bindingSnapshot)setSavedBindingOverrides(options.bindingSnapshot);
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
    } finally {
      transferInFlight.current=false;
    }
  }

  async function installBuiltinToDevice(component,options={}) {
    const id = component?.id || component;
    const clawpkgPath = await invoke("resolve_builtin_clawpkg_path", { id });
    return startOtaInstall(id, clawpkgPath, { component,...options });
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
    installEpoch.current++;
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
    }
  }

  return (
    <PageShell
      title="组件中心"
      subtitle="发现、创作与分享属于你的桌面玩法"
      help={onboarding.show}
      actions={[
        <button
          key="refresh-component-library"
          type="button"
          className="btn-ghost btn-sm"
          onClick={() => {
            setCommunityRefreshKey((key) => key + 1);
            refreshComponentLibrary();
            refreshDeviceInventory({ silent: true });
          }}
          disabled={componentLibraryLoading || inventoryLoading}
        >
          <RefreshCw size={14} />
          {componentLibraryLoading || inventoryLoading ? "刷新中…" : "刷新组件库"}
        </button>,
        <button
          key="create-component"
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
            description: "未同步组件可直接下发；已同步组件可继续同步更新，删除入口可选择仅从设备删除。",
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
      <CommunityCenter
        active={active}
        tab={centerTab}
        onTabChange={setCenterTab}
        refreshKey={communityRefreshKey}
        localItems={libraryItems}
        inventory={deviceInventory}
        onOpenLocal={openComponent}
        onCreate={() => setCreateDrawerOpen(true)}
        onImported={() => refreshComponentLibrary()}
      >
      {componentLibraryError&&<div role="alert" className="community-error">本机组件库读取失败，保留上次列表。{componentLibraryError}<button onClick={refreshComponentLibrary}>重试</button></div>}
      {deviceInventory.freshness!=='live'&&<p role="status">{deviceConnected?'设备清单待核对；安装状态暂不确定。':'设备未连接；仍可管理本机组件。'}</p>}
      {deviceInventory.warnings?.map(w=><p role="alert" className="community-error" key={w}>{w}</p>)}
      {/* ── one library: local availability and live board state ─────────── */}
      <section
        ref={librarySectionRef}
        id="component-library"
        className="component-library-section"
      >
        <div className="component-library-toolbar">
          <button className="btn-secondary btn-sm" onClick={()=>setTrashOpen(true)}><Trash2 size={14}/>回收站</button>
          <input aria-label="搜索我的组件" placeholder="搜索名称、作者" value={libraryQuery} onChange={e=>setLibraryQuery(e.target.value)}/>
          <select aria-label="组件状态筛选" value={libraryStatus} onChange={e=>setLibraryStatus(e.target.value)}>
            <option value="all">所有状态</option><option value="installed">已安装</option><option value="update">可更新</option><option value="pc">需要 PC</option><option value="offline">可离线运行</option>
          </select>
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
        {!libraryReady&&<p role="status">正在读取我的组件…</p>}
        <div className="component-library-grid" hidden={!libraryReady} style={!libraryReady?{display:"none"}:undefined}>
          {filteredLibraryItems.map((item) => {
            const isEnabled = Boolean(
              deviceInventory.freshness === "live"
              && deviceInventory.activeWidgetId === item.id
              && enabledComponentSourceKey
              && componentSourceKey(item) === enabledComponentSourceKey,
            );
            const isInstalled = deviceInventory.freshness === "live" && installedIds.has(item.id);
            return (
              <CandidateCard
                key={item.id}
                component={item}
                kind={resolveComponentKind(item.kind, item.gameType)}
                isLocal={item.isLocal}
                isInstalled={isInstalled}
                isEnabled={isEnabled}
                deviceState={devicePackageState(item,deviceInventory)}
                onClick={() => openComponent(item)}
                onDeviceAction={() => {
                  openComponent(item);
                }}
                onDelete={
                  item.isLocal || isInstalled
                    ? () => requestComponentDelete(item, isInstalled)
                    : undefined
                }
              />
            );
          })}
          <CreateNewCard onClick={() => setCreateDrawerOpen(true)} />
        </div>
        {libraryReady&&filteredLibraryItems.length === 0 && (
          <div className="component-library-empty" role="status">
            <LayoutGrid size={22} aria-hidden="true" />
            <strong>该分类暂无组件</strong>
            <span>更换搜索词，或清除分类与状态筛选。</span>
            <button onClick={()=>{setLibraryKind('all');setLibraryStatus('all');setLibraryQuery('');}}>清除筛选</button>
          </div>
        )}
      </section>
      </CommunityCenter>

      {/* ── modals ──────────────────────────────────────────────────────── */}
      {trashOpen&&<RemovedComponents onClose={()=>setTrashOpen(false)} onRestored={refreshComponentLibrary}/>}

      {previewComponent && isComponentVisible(previewComponent) && (
        <ComponentPreviewModal
          component={previewComponent}
          onSourceChange={openComponent}
          usb={usb}
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
          globalExitControl={
            globalExitEvents
              .map(globalExitControlLabel)
              .filter(Boolean)
              .join("、") || "未设置"
          }
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
          onDelete={
            previewComponent.isLocal || previewIsInstalled
              ? () => {
                  const component = previewComponent;
                  setPreviewComponent(null);
                  requestComponentDelete(component, previewIsInstalled);
                }
              : undefined
          }
          onClose={closeComponent}
        />
      )}

      {otaPhase !== "idle" && (
        <InstallDialog busy={otaPhase==='installing'} onClose={dismissOtaModal}>
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
                <p>{otaProgress ? `设备已接收 ${otaProgress.current}/${otaProgress.total} 个文件，等待完整校验与提交回执…` : "正在校验资源包并连接设备，请保持 USB 连接…"}</p>
                <progress aria-label="组件同步进度" max={otaProgress?.total || 1} value={otaProgress?.current} />
                <p>只有设备确认提交成功后才会显示安装完成。</p>
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
                    const retry={...otaPendingOptions,epoch:++installEpoch.current};setOtaPendingOptions(retry);
                    performOtaInstall(localId || builtinId || "", otaPendingPath, retry);
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
        </InstallDialog>
      )}

      {downgradeComponent&&<WorkflowConfirm title="设备版本更高" description={`继续会使用当前选择的 ${downgradeComponent.version} 覆盖设备上的「${downgradeComponent.name}」资源。请确认是否需要降级。`} confirmLabel="确认降级并同步" onCancel={()=>setDowngradeComponent(null)} onConfirm={()=>{const component=downgradeComponent;setDowngradeComponent(null);syncSelectedComponent(component,true);}}/>}
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
              {pendingComponentAction.installedOnDevice && pendingComponentAction.isLocal
                ? "选择删除范围"
                : pendingComponentAction.installedOnDevice
                  ? "设备组件"
                  : "电脑组件库"}
            </span>
            <h2 id="component-action-confirm-title">
              {`删除“${pendingComponentAction.component.name || pendingComponentAction.component.id}”？`}
            </h2>
            <p id="component-action-confirm-description">
              {pendingComponentAction.isLocal&&'本机移除会将该组件全部历史资源版本放入可恢复的回收站；用户记录和按键设置保留，内置副本仍可使用。'}
              {pendingComponentAction.installedOnDevice && pendingComponentAction.isLocal
                ? "可以只移除设备副本，也可以等待设备确认后，再将本机源移入回收站。"
                : pendingComponentAction.installedOnDevice
                  ? pendingComponentAction.wasActive
                    ? "这会删除设备上正在运行的组件并返回主屏；PC 内置组件库不受影响。"
                    : "这只会删除设备保存的组件副本；PC 组件库不受影响。"
                  : "本次仅移除本机资源，不操作设备；设备是否安装请以最近核对结果为准。"}
              {pendingComponentAction.component.isDeviceOnly&&' 本机没有安装源；移除这个设备副本后，不能通过本机回收站恢复，请确认已另存资源包。'}
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
              {pendingComponentAction.installedOnDevice && (
                <button
                  className="btn-secondary danger"
                  type="button"
                  onClick={() => confirmComponentAction("device-only")}
                  disabled={componentActionPending}
                >
                  <Unplug size={15} />
                  {componentActionPending ? "处理中…" : "仅从设备删除"}
                </button>
              )}
              {pendingComponentAction.isLocal && (
                <button
                  className="btn-ghost danger"
                  type="button"
                  onClick={() => confirmComponentAction("delete-library")}
                  disabled={componentActionPending}
                >
                  <Trash2 size={15} />
                  {componentActionPending
                    ? "处理中…"
                    : pendingComponentAction.installedOnDevice
                      ? "移除设备并回收本机源"
                      : "移入本机回收站"}
                </button>
              )}
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
          componentLibraryLoading={componentLibraryLoading}
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

function InstallDialog({busy,onClose,children}){
  const ref=useDialogFocus(onClose,busy);
  return <div ref={ref} tabIndex={-1} className="component-replace-modal" role="dialog" aria-modal="true" aria-label="同步到设备">{children}</div>;
}

function CreateComponentDrawer({
  onClose,
  handleInstallSkill,
  skillInstalling,
  skillInstallResult,
  componentLibraryLoading,
  refreshComponentLibrary,
}) {
  const [invocationCopied, setInvocationCopied] = useState(false);
  const [copyError,setCopyError]=useState('');
  const drawerRef=useDialogFocus(onClose,skillInstalling);
  const invocationExample = "请使用 $petui，帮我生成一个桌搭子组件：<宠物养成小游戏，一个类似旅行青蛙的可爱小青蛙形象，1键是抚摸、亲密度+1，2键是喂食、亲密度+5，摇杆上下左右可让它到处移动> （示意，请按照自己想法重新输入）";

  async function copyInvocationExample() {
    try {
      await navigator.clipboard.writeText(invocationExample);
      setCopyError('');
      setInvocationCopied(true);
      window.setTimeout(() => setInvocationCopied(false), 1800);
    } catch (error) {
      console.warn("[ComponentCenter] copy petui invocation failed", error);
      setCopyError('复制失败，请手动选择下方示例文字复制。');
    }
  }

  return (
    <div
      className="component-center-drawer-backdrop"
      role="presentation"
      onClick={()=>{if(!skillInstalling)onClose();}}
    >
      <aside
        ref={drawerRef}
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
            disabled={skillInstalling}
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
            <h3>打开你正在使用的 Agent</h3>
            <p>复制下方示例文案到Agent界面，与Agent对话生成你想要在设备上使用的小组件或小游戏</p>
          </header>
          <code className="component-petui-invocation">{invocationExample}</code>
          {copyError&&<p role="alert">{copyError}</p>}
          <button className="btn-secondary" type="button" onClick={copyInvocationExample}>
            <Clipboard size={15} />
            {invocationCopied ? "已复制" : "复制调用示例"}
          </button>
        </article>

        <article className="component-tool-card component-tool-card--refresh">
          <header>
            <span className="component-tool-eyebrow">STEP 3 · 刷新组件库</span>
            <h3>Agent制作完成后，刷新组件库</h3>
            <p>刷新后新组件会出现在组件中心</p>
          </header>
          <button
            type="button"
            className="btn-secondary"
            onClick={refreshComponentLibrary}
            disabled={componentLibraryLoading}
          >
            <RefreshCw size={15} />
            {componentLibraryLoading ? "刷新中…" : "刷新组件库"}
          </button>
        </article>
      </aside>
    </div>
  );
}
