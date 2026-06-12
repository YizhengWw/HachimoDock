/**
 * [Input] Consume component-center package fixtures from `ref/src/fixtures.js`[Pos].
 * [Output] Component store: NowShowingHero at top showing installed component, flat library
 *          grid of CandidateCards below with complete custom draft summaries, ComponentPreviewModal as the final install confirmation. Preserves
 *          all OTA install / draft delete / legacy replace-confirm modals and the
 *          CreateComponentDrawer copy that generated components auto-refresh in the center and a
 *          drag-or-pick fallback button manually adds a package when the draft is not visible.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  Clipboard,
  PackageCheck,
  Plus,
  RefreshCw,
  RotateCcw,
  Settings2,
  Sparkles,
  Trash2,
  X,
} from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import { BUILTIN_COMPONENT_CENTER } from "./fixtures";
import {
  createSkillTriggerPrompt,
  labelForComponentGenerationAgent,
  loadFollowedComponentGenerationAgentId,
} from "./lib/component-generation-template.js";
import PageShell from "./shell/PageShell.jsx";
import { useToast } from "./shell/ToastStack.jsx";
import { useDeviceContext } from "./shell/DeviceContext.jsx";
import NowShowingHero from "./component-center/NowShowingHero.jsx";
import CandidateCard from "./component-center/CandidateCard.jsx";
import ComponentPreviewModal from "./component-center/ComponentPreviewModal.jsx";
import { isRoutedWidgetBinding } from "./component-center/binding-labels.js";

const ACTIVE_COMPONENT_STORAGE_KEY = "pet-manager:active-component";

/* Controls a widget action can be bound to at install time. Each maps to a
 * NATIVE board event the widget runtime matches in buttons.json. Screen region
 * events are forwarded by the touch runtime when the widget is active. Screen
 * swipe stays a pure system page-switch gesture and is intentionally NOT
 * bindable here. Knob rotation is fixed to system volume and is not offered as
 * a live widget control in this UI.
 *
 * 与设备端 board-runtime 同步：负一屏路由见 board_rotary_input.c
 * br_rotary_dispatch_button_action 与 board_touch_input.c；swipe
 * 仍由 screen_page.c 截走切页，因此不在可绑定列表里。
 */
const CONTROL_OPTIONS = [
  {
    label: "屏幕点击",
    shortLabel: "点击",
    control: "屏幕区域",
    event: "screen.region.tap",
    help: "点 stats 页屏幕区域触发这个组件动作。",
  },
  {
    label: "屏幕长按",
    shortLabel: "长按",
    control: "屏幕区域",
    event: "screen.region.long_press",
    help: "长按 stats 页屏幕区域触发这个组件动作。",
  },
];

const CONTROL_HELP = Object.fromEntries(
  CONTROL_OPTIONS.map((option) => [option.label, option.help]),
);

function bindingKey(componentId, action) {
  return `${componentId}:${action}`;
}

function defaultControlLabelForBinding(binding) {
  const event = binding.event || "";
  const control = binding.control || "";
  const exactMatch = CONTROL_OPTIONS.find(
    (option) => option.control === control && option.event === event,
  );
  if (exactMatch) return exactMatch.label;
  const eventMatch = CONTROL_OPTIONS.find((option) => option.event === event);
  if (eventMatch) return eventMatch.label;
  const controlMatch = CONTROL_OPTIONS.find((option) => option.control === control);
  return controlMatch?.label || control || CONTROL_OPTIONS[0].label;
}

function optionForControlLabel(label) {
  return CONTROL_OPTIONS.find((option) => option.label === label) || null;
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

function buildDraftGoal(draft) {
  const description = typeof draft.description === "string" ? draft.description.trim() : "";
  return description || "自定义草稿 · 可预览后安装到负一屏。";
}

function normalizeLocalPath(value) {
  return String(value || "").replaceAll("\\", "/");
}

function pathContainsComponentId(value, componentId) {
  const id = String(componentId || "").trim();
  if (!id) return false;
  return normalizeLocalPath(value)
    .split("/")
    .some((segment) => segment === id || segment === `${id}.clawpkg` || segment === `${id}.zip`);
}

function matchesDraftPath(draft, clawpkgPath) {
  if (!draft || !clawpkgPath) return false;
  return normalizeLocalPath(draft.path) === normalizeLocalPath(clawpkgPath)
    || pathContainsComponentId(clawpkgPath, draft.id);
}

export default function ComponentCenter() {
  const { push } = useToast();
  const { usb, currentComponent: deviceCurrentComponent } = useDeviceContext();
  const deviceConnected = usb.connected;

  const [createDrawerOpen, setCreateDrawerOpen] = useState(false);
  const [previewComponent, setPreviewComponent] = useState(null);
  const [installedIds, setInstalledIds] = useState(() =>
    new Set(
      BUILTIN_COMPONENT_CENTER.components
        .filter((c) => c.status === "installed")
        .map((c) => c.id),
    ),
  );
  const [activeNegativeScreenId, setActiveNegativeScreenId] = useState(
    deviceCurrentComponent?.id || "focus-flow",
  );
  const [bindingOverrides, setBindingOverrides] = useState({});
  const [promptDraft, setPromptDraft] = useState(BUILTIN_COMPONENT_CENTER.promptBuilder.defaultPrompt);
  const [followedAgentId] = useState(loadFollowedComponentGenerationAgentId);
  const [showReplaceConfirm, setShowReplaceConfirm] = useState(false);
  const [skillInstalling, setSkillInstalling] = useState(false);
  const [skillInstallResult, setSkillInstallResult] = useState(null);
  const [clawpkgDragOver, setClawpkgDragOver] = useState(false);
  const [clawpkgImporting, setClawpkgImporting] = useState(false);
  const [clawpkgImportResult, setClawpkgImportResult] = useState(null);
  const [drafts, setDrafts] = useState([]);
  const [draftsLoading, setDraftsLoading] = useState(false);
  const [deleteDraftPath, setDeleteDraftPath] = useState(null);
  const [deleteDraftDeleting, setDeleteDraftDeleting] = useState(false);
  const [deleteDraftError, setDeleteDraftError] = useState("");
  /* USB OTA install flow modal: idle | checking-usb | waiting-usb | installing | success | error */
  const [otaPhase, setOtaPhase] = useState("idle");
  const [otaPendingPath, setOtaPendingPath] = useState(null);
  const [otaPendingOptions, setOtaPendingOptions] = useState({});
  const [otaTargetName, setOtaTargetName] = useState("");
  const [otaError, setOtaError] = useState(null);
  const [otaResult, setOtaResult] = useState(null);

  const refreshDrafts = useCallback(async () => {
    setDraftsLoading(true);
    try {
      const list = await invoke("list_component_drafts");
      setDrafts(Array.isArray(list) ? list : []);
    } catch (err) {
      console.warn("[ComponentCenter] list_component_drafts failed", err);
      setDrafts([]);
    } finally {
      setDraftsLoading(false);
    }
  }, []);

  /* auto-scan on mount + poll every 30s while page is open */
  useEffect(() => {
    refreshDrafts();
    const interval = setInterval(refreshDrafts, 30000);
    return () => clearInterval(interval);
  }, [refreshDrafts]);

  /** Build a component-like object for a draft for consistent shape consumption */
  function buildDraftAsComponent(draft) {
    return {
      id: draft.id,
      name: draft.name || draft.id,
      goal: buildDraftGoal(draft),
      dashboard: draft.dashboard || {},
      defaultBindings: [],
      screens: [{ name: "负一屏", purpose: "draft 自带按钮功能绑定", regions: [] }],
      status: "draft",
      draftPath: draft.path,
      isDraft: true,
    };
  }

  /** currentComponent from useDeviceContext gives {id, name} only.
   *  Look up the full object from builtins or drafts. Fall back to minimal shape. */
  const currentFullComponent = useMemo(() => {
    const id = deviceCurrentComponent?.id || activeNegativeScreenId;
    if (!id) return null;
    const builtin = BUILTIN_COMPONENT_CENTER.components.find((c) => c.id === id);
    if (builtin) return builtin;
    const draft = drafts.find((d) => d.id === id);
    if (draft) return buildDraftAsComponent(draft);
    // External / unknown: return minimal shape so hero can still render name
    if (deviceCurrentComponent) return deviceCurrentComponent;
    return null;
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [deviceCurrentComponent, activeNegativeScreenId, drafts]);

  /** Library items = all builtins + tagged drafts, minus the currently-installed component */
  const libraryItems = useMemo(() => {
    const currentId = currentFullComponent?.id;
    const builtins = BUILTIN_COMPONENT_CENTER.components.map((item) => ({ ...item, isDraft: false }));
    const draftItems = drafts.map((d) => ({ ...buildDraftAsComponent(d), isDraft: true }));
    return [...builtins, ...draftItems].filter((item) => item.id !== currentId);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [currentFullComponent, drafts]);

  const activeNegativeScreen = useMemo(() => {
    const builtin = BUILTIN_COMPONENT_CENTER.components.find((c) => c.id === activeNegativeScreenId);
    if (builtin) return builtin;
    const draft = drafts.find((d) => d.id === activeNegativeScreenId);
    if (draft) return buildDraftAsComponent(draft);
    if (activeNegativeScreenId) return { id: activeNegativeScreenId, name: "已删除的自定义组件" };
    return BUILTIN_COMPONENT_CENTER.components[0];
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeNegativeScreenId, drafts]);

  const pendingDeleteDraft = useMemo(
    () => drafts.find((draft) => draft.path === deleteDraftPath) || null,
    [deleteDraftPath, drafts],
  );

  const [libraryHighlighted, setLibraryHighlighted] = useState(false);
  const librarySectionRef = useRef(null);

  const followedAgentLabel = labelForComponentGenerationAgent(followedAgentId);

  const handleBrowseLibrary = useCallback(() => {
    librarySectionRef.current?.scrollIntoView({ behavior: "smooth", block: "start" });
    setLibraryHighlighted(true);
    const t = setTimeout(() => setLibraryHighlighted(false), 1500);
    // cleanup happens naturally if user re-clicks; previous timer just fires harmlessly
  }, []);

  async function installSelectedComponent(component = previewComponent) {
    if (!component) return;
    setInstalledIds((current) => {
      const next = new Set(current);
      next.add(component.id);
      return next;
    });
    setActiveNegativeScreenId(component.id);
    setShowReplaceConfirm(false);
    setPreviewComponent(null);

    // Plan 4: write the active component so the sidebar ContextRail (Plan 1) updates.
    // DeviceContext.jsx already listens to the "storage" event and re-reads this key.
    try {
      localStorage.setItem(
        ACTIVE_COMPONENT_STORAGE_KEY,
        JSON.stringify({ id: component.id, name: component.name }),
      );
      window.dispatchEvent(new Event("storage"));
    } catch (err) {
      console.warn("[ComponentCenter] failed to persist active component", err);
    }

    /* Drafts ship their own buttons.json + dashboard from the agent. Builtins
       resolve through the Tauri backend so packaged apps use bundled resources
       instead of assuming ~/.openclaw exists on every user's machine. */
    if (component.draftPath) {
      await installClawpkgFromPath(component.draftPath, {
        targetName: component.name,
        skipFooterOverride: true,
      });
    } else {
      await installBuiltinToDevice(component.id);
    }

  }

  async function handleGenerateClick() {
    /* prefer the short skill-trigger prompt — relies on user having Step 1
       installed petAgent-ui-generator, which auto-loads its full SKILL.md when
       triggered. Falls back to the inlined long template only as a safety. */
    const prompt = createSkillTriggerPrompt(promptDraft);
    try {
      await invoke("launch_agent_with_prompt", { input: { agentId: followedAgentId, prompt } });
    } catch (err) {
      console.error("[ComponentCenter] launch_agent_with_prompt failed", err);
      const msg = typeof err === "string" ? err : String(err);
      push({ tone: "error", title: "生成组件启动失败", message: msg });
    }
  }

  function requestDeleteDraft(draft) {
    setDeleteDraftPath(draft.path || draft.draftPath);
    setDeleteDraftError("");
  }

  function cancelDeleteDraft() {
    if (deleteDraftDeleting) return;
    setDeleteDraftPath(null);
    setDeleteDraftError("");
  }

  async function confirmDeleteDraft() {
    if (!deleteDraftPath) return;
    const targetPath = deleteDraftPath;
    const targetDraft = drafts.find((draft) => draft.path === targetPath);
    setDeleteDraftDeleting(true);
    setDeleteDraftError("");
    try {
      await invoke("delete_component_draft", { input: { path: targetPath } });
      setDrafts((current) => current.filter((draft) => draft.path !== targetPath));
      if (targetDraft?.id) {
        setInstalledIds((current) => {
          const next = new Set(current);
          next.delete(targetDraft.id);
          return next;
        });
        if (activeNegativeScreenId === targetDraft.id) {
          setActiveNegativeScreenId(BUILTIN_COMPONENT_CENTER.components[0]?.id || "");
        }
      }
      setDeleteDraftPath(null);
      await refreshDrafts();
    } catch (err) {
      setDeleteDraftError(typeof err === "string" ? err : String(err));
    } finally {
      setDeleteDraftDeleting(false);
    }
  }

  function resolveControlOption(binding, component) {
    const key = bindingKey(component?.id || "", binding.action);
    const label = bindingOverrides[key] || defaultControlLabelForBinding(binding);
    return optionForControlLabel(label) || {
      label,
      shortLabel: label,
      control: binding.control || label,
      event: binding.event || "",
      help: CONTROL_HELP[label] || "安装时会把这个组件动作写入 buttons.json。",
    };
  }

  function buildBindingOverridesForInstall(component) {
    if (!component || !Array.isArray(component.defaultBindings)) return {};
    return component.defaultBindings.reduce((overrides, binding) => {
      const selectedLabel = bindingOverrides[bindingKey(component.id, binding.action)];
      if (selectedLabel && selectedLabel !== defaultControlLabelForBinding(binding)) {
        overrides[binding.action] = selectedLabel;
      }
      return overrides;
    }, {});
  }

  function updateBinding(binding, nextControl, component) {
    setBindingOverrides((current) => ({
      ...current,
      [bindingKey(component.id, binding.action)]: nextControl,
    }));
  }

  function resetBindings(component) {
    setBindingOverrides((current) => {
      const next = { ...current };
      (component?.defaultBindings || []).forEach((binding) => {
        delete next[bindingKey(component.id, binding.action)];
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
    const sshHost = (typeof window !== "undefined" && window.localStorage)
      ? (window.localStorage.getItem("petManager.sshHost") || "").trim()
      : "";
    if (sshHost && !options.forceUsb) {
      await performOtaInstall(componentId, clawpkgPath, options);
      return;
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
      return;
    }
    await performOtaInstall(componentId, clawpkgPath, options);
  }

  async function performOtaInstall(componentId, clawpkgPath, options = {}) {
    const component = BUILTIN_COMPONENT_CENTER.components.find((c) => c.id === componentId);
    setOtaPhase("installing");

    /* Transport dispatch: if user has set `petManager.sshHost` in localStorage
       (e.g. "petagent@<DEVICE_IP>"), go over SSH directly to the LAN-attached device.
       Otherwise fall back to the original USB-serial transport. SSH transport
       passes binding overrides through so the device-side widget runtime gets
       the user's customized buttons.json (action → control remapping) baked in. */
    const sshHost = (typeof window !== "undefined" && window.localStorage)
      ? window.localStorage.getItem("petManager.sshHost") || ""
      : "";
    const useSsh = !options.forceUsb && sshHost.trim().length > 0;
    try {
      let result;
      if (useSsh) {
        result = await invoke("install_clawpkg_over_ssh", {
          input: {
            clawpkgPath,
            sshHost: sshHost.trim(),
            bindingOverrides: buildBindingOverridesForInstall(component),
          },
        });
      } else {
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
        setOtaError(`校验失败: ${result.errors.join("; ")}`);
        setOtaPhase("error");
        push({ tone: "error", title: "安装失败", message: result.errors.join("; ") });
        return;
      }
      setOtaResult(result);
      setOtaPhase("success");
      push({
        tone: "success",
        title: `已推送到设备 · ${result.manifest?.name || otaTargetName}`,
      });
    } catch (err) {
      const msg = typeof err === "string" ? err : String(err);
      if (!useSsh && (msg.includes("USB 未连接") || msg.includes("USB not connected"))) {
        setOtaPhase("waiting-usb");
      } else {
        setOtaError(msg);
        setOtaPhase("error");
        push({ tone: "error", title: "安装失败", message: msg });
      }
    }
  }

  async function installBuiltinToDevice(id) {
    const clawpkgPath = await invoke("resolve_builtin_clawpkg_path", { id });
    await startOtaInstall(id, clawpkgPath);
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
          const draftId = drafts.find(
            (d) => matchesDraftPath(d, otaPendingPath),
          )?.id;
          const builtinId = BUILTIN_COMPONENT_CENTER.components.find(
            (c) => otaPendingPath && otaPendingPath.includes(c.id),
          )?.id;
          await performOtaInstall(draftId || builtinId || "", otaPendingPath, otaPendingOptions);
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
      const draftMatch = drafts.find((d) => matchesDraftPath(d, clawpkgPath));
      const builtinMatch = BUILTIN_COMPONENT_CENTER.components.find((c) => clawpkgPath.includes(c.id));
      const guessedId = draftMatch?.id || builtinMatch?.id || currentFullComponent?.id || "";
      const resolvedOptions = draftMatch
        ? { targetName: draftMatch.name, skipFooterOverride: true, ...options }
        : options;
      await startOtaInstall(guessedId, clawpkgPath, resolvedOptions);
    } catch (err) {
      const msg = typeof err === "string" ? err : String(err);
      push({ tone: "error", title: "安装 .clawpkg 失败", message: msg });
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
    await installClawpkgFromPath(localPath);
  }

  async function handleClawpkgFilePick() {
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".clawpkg,.zip";
    input.onchange = (e) => {
      const file = e.target.files && e.target.files[0];
      if (file && file.path) {
        installClawpkgFromPath(file.path);
      } else {
        push({
          tone: "error",
          title: "无法获取本地路径",
          message: "浏览器模式下没有本地路径访问。请用桌面应用模式。",
        });
      }
    };
    input.click();
  }

  return (
    <PageShell
      title="组件中心"
      subtitle="桌搭子的负一屏组件"
      actions={[
        <button
          key="refresh-drafts"
          type="button"
          className="btn-ghost btn-sm"
          onClick={refreshDrafts}
          disabled={draftsLoading}
        >
          <RefreshCw size={14} />
          {draftsLoading ? "扫描中…" : "刷新草稿"}
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
      {/* ── hero: currently-installed component ─────────────────────────── */}
      <NowShowingHero
        component={currentFullComponent}
        buttonBindings={currentFullComponent?.defaultBindings?.filter(isRoutedWidgetBinding).slice(0, 3) || []}
        deviceConnected={deviceConnected}
        onChangeRequest={handleBrowseLibrary}
      />

      {/* ── library: flat grid of all other components ───────────────────── */}
      <section
        ref={librarySectionRef}
        id="component-library"
        className={`component-library-section${libraryHighlighted ? " is-highlighted" : ""}`}
      >
        <header>
          <h2>组件库</h2>
          <small>{libraryItems.length} 个可选</small>
        </header>
        <div className="component-library-grid">
          {libraryItems.map((item) => (
            <CandidateCard
              key={item.isDraft ? item.draftPath || item.id : item.id}
              component={item}
              isDraft={item.isDraft}
              onClick={() => setPreviewComponent(item)}
              onDelete={item.isDraft ? () => requestDeleteDraft(item) : undefined}
            />
          ))}
          <CreateNewCard onClick={() => setCreateDrawerOpen(true)} />
        </div>
      </section>

      {/* legacy panel hidden — content lives in hero + modal now */}
      <section className="component-device-panel" style={{ display: "none" }} />

      {/* ── modals ──────────────────────────────────────────────────────── */}

      {previewComponent && (
        <ComponentPreviewModal
          component={previewComponent}
          isDraft={previewComponent.isDraft}
          currentComponent={currentFullComponent}
          deviceConnected={deviceConnected}
          installing={otaPhase === "installing" || clawpkgImporting}
          onInstall={() => installSelectedComponent(previewComponent)}
          onClose={() => setPreviewComponent(null)}
        />
      )}

      {otaPhase !== "idle" && (
        <div className="component-replace-modal" role="dialog" aria-modal="true" aria-label="USB 安装到设备">
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
                    const draftId = drafts.find(
                      (d) => matchesDraftPath(d, otaPendingPath),
                    )?.id;
                    const builtinId = BUILTIN_COMPONENT_CENTER.components.find(
                      (c) => otaPendingPath.includes(c.id),
                    )?.id;
                    performOtaInstall(draftId || builtinId || "", otaPendingPath, otaPendingOptions);
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

      {deleteDraftPath && (
        <div className="component-replace-modal" role="dialog" aria-modal="true" aria-label="删除组件">
          <section>
            <span>删除组件</span>
            <h2>删除"{pendingDeleteDraft?.name || "这个自定义组件"}"？</h2>
            <p>
              会从本机组件草稿目录删除这个自定义组件。
              {pendingDeleteDraft?.id === activeNegativeScreenId
                ? " 它已经推到设备时，只删除本机草稿，不会清空设备负一屏。"
                : ""}
            </p>
            {deleteDraftError && <p className="delete-draft-error">{deleteDraftError}</p>}
            <div>
              <button className="btn-secondary" type="button" onClick={cancelDeleteDraft} disabled={deleteDraftDeleting}>
                取消
              </button>
              <button className="btn-ghost danger" type="button" onClick={confirmDeleteDraft} disabled={deleteDraftDeleting}>
                <Trash2 size={15} />
                {deleteDraftDeleting ? "删除中…" : "确认删除"}
              </button>
            </div>
          </section>
        </div>
      )}

      {showReplaceConfirm && previewComponent && (
        <div className="component-replace-modal" role="dialog" aria-modal="true" aria-label="替换负一屏确认">
          <section>
            <span>替换负一屏</span>
            <h2>用"{previewComponent.name}"替换"{activeNegativeScreen.name}"？</h2>
            <p>设备上的负一屏页面和按钮功能绑定会切到新组件。旧组件会留在已安装列表，之后可以恢复。</p>
            <div>
              <button className="btn-secondary" type="button" onClick={() => setShowReplaceConfirm(false)}>
                取消
              </button>
              <button className="btn-primary" type="button" onClick={() => installSelectedComponent(previewComponent)}>
                确认替换
              </button>
            </div>
          </section>
        </div>
      )}

      {createDrawerOpen && (
        <CreateComponentDrawer
          onClose={() => setCreateDrawerOpen(false)}
          followedAgentId={followedAgentId}
          followedAgentLabel={followedAgentLabel}
          promptDraft={promptDraft}
          setPromptDraft={setPromptDraft}
          handleGenerateClick={handleGenerateClick}
          handleInstallSkill={handleInstallSkill}
          skillInstalling={skillInstalling}
          skillInstallResult={skillInstallResult}
          clawpkgDragOver={clawpkgDragOver}
          setClawpkgDragOver={setClawpkgDragOver}
          handleClawpkgDrop={handleClawpkgDrop}
          handleClawpkgFilePick={handleClawpkgFilePick}
          clawpkgImporting={clawpkgImporting}
          clawpkgImportResult={clawpkgImportResult}
        />
      )}
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
        <p className="candidate-card__goal">用 AI 生成，完成后自动刷新；没看到就拖拽加入。</p>
      </div>
    </button>
  );
}

function CreateComponentDrawer({
  onClose,
  followedAgentId,
  followedAgentLabel,
  promptDraft,
  setPromptDraft,
  handleGenerateClick,
  handleInstallSkill,
  skillInstalling,
  skillInstallResult,
  clawpkgDragOver,
  setClawpkgDragOver,
  handleClawpkgDrop,
  handleClawpkgFilePick,
  clawpkgImporting,
  clawpkgImportResult,
}) {
  useEffect(() => {
    function onKey(event) {
      if (event.key === "Escape") onClose();
    }
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);

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
            <p>3 步走：装 skill → 用自然语言生成 → 生成完成后组件中心会自动刷新并展示新草稿；没看到更新时，用 Step 3 拖拽或选择加入组件中心。</p>
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
            <h3>把 petAgent-ui-generator 装到检测到的 Coding Agent</h3>
            <p>自动扫描 <code>~/.claude/</code> · <code>~/.codex/</code> · <code>~/.openclaw/</code> · <code>~/.gemini/</code> · <code>~/.cursor/</code>,把 skill 装到每个检测到的 agent。装好后任一会话里说"做个桌搭子组件"自动触发。</p>
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
            <span className="component-tool-eyebrow">STEP 2 · 描述生成</span>
            <h3>用文字描述,直接生成</h3>
            <p>装好 Skill 后,点按钮会调起当前跟随的 <code>{followedAgentLabel}</code> terminal,把你的描述喂进去,skill 自动接管生成组件。生成完成后组件中心会自动刷新并展示新草稿。说明切到负一屏后的默认场景,再把点击 / 长按分别做什么说清楚；没说清楚时 skill 会先追问。</p>
          </header>
          <textarea
            className="component-generate-textarea"
            aria-label="搜索或描述想要的组件"
            value={promptDraft}
            onChange={(event) => setPromptDraft(event.target.value)}
            placeholder={BUILTIN_COMPONENT_CENTER.promptBuilder.placeholder}
          />
          <p className="component-generate-guidance">
            可写:组件用途、切到负一屏后的默认场景和自运行状态、显示哪些数字/状态、点击 screen.region.tap 做什么、长按 screen.region.long_press 做什么；旋钮固定用于音量。
          </p>
          <button className="btn-primary" type="button" onClick={handleGenerateClick}>
            <Sparkles size={15} />
            生成组件
          </button>
        </article>

        <article className="component-tool-card component-tool-card--clawpkg">
          <header>
            <span className="component-tool-eyebrow">STEP 3 · 自动更新 / 手动加入</span>
            <h3>没看到更新时再拖拽加入</h3>
            <p>生成完成后组件中心会自动刷新并展示新草稿；如果没看到更新，把生成出的 <code>.clawpkg</code> 目录或 zip 拖到这里，或点按钮选择文件手动加入。</p>
          </header>
          <div
            className={`component-clawpkg-dropzone ${clawpkgDragOver ? "is-dragover" : ""}`}
            onDragOver={(e) => { e.preventDefault(); setClawpkgDragOver(true); }}
            onDragLeave={() => setClawpkgDragOver(false)}
            onDrop={handleClawpkgDrop}
          >
            <Clipboard size={20} />
            <span>{clawpkgImporting ? "正在校验 + 推送到设备…" : "拖拽 .clawpkg 目录 / zip 到这里"}</span>
          </div>
          <button
            type="button"
            className="btn-secondary component-clawpkg-pick-button component-clawpkg-fallback-button"
            onClick={handleClawpkgFilePick}
            disabled={clawpkgImporting}
          >
            <Clipboard size={15} />
            拖拽或选择加入组件中心
          </button>
          {clawpkgImportResult && (
            <p className="component-tool-result__inline">
              已安装: <strong>{clawpkgImportResult.manifest.name}</strong>（{clawpkgImportResult.transferredBytes} bytes）
            </p>
          )}
        </article>
      </aside>
    </div>
  );
}
