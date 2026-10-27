/**
 * [Input] Consume DeviceSetup.jsx and DeviceDashboard.jsx.
 * [Output] Always-accessible desktop navigation, retryable USB setup, retained API drafts and explicitly active component observers. Accessibility consent is requested by native Agent operations, not app launch.
 * [Pos] component node in pc/src
 * Generation notifications distinguish reference-image rejection with retained partial results from full success.
 * [Sync] If this file changes, update this header and `pc/src/.folder.md`.
 */

import React, { useState, useEffect, useCallback, useRef, lazy, Suspense } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  ImagePlus,
  Loader2,
  MonitorSmartphone,
  Blocks,
  KeyRound,
  Home,
} from "lucide-react";
import DeviceSetup from "./DeviceSetup";
import DeviceDashboard from "./DeviceDashboard";
const AppearanceGallery = lazy(() => import("./AppearanceGallery"));
const CustomAvatarWizard = lazy(() => import("./CustomAvatarWizard"));
const AppearanceDetail = lazy(() => import("./AppearanceDetail"));
const ComponentCenter = lazy(() => import("./ComponentCenter"));
import ComponentCenterBoundary from "./component-center/ComponentCenterBoundary.jsx";
const ApiSettings = lazy(() => import("./ApiSettings"));
const SmartHome = lazy(() => import("./SmartHome.jsx"));
import { DeviceContextProvider, useDeviceContext } from "./shell/DeviceContext.jsx";
import ToastStack, { ToastProvider, useToast } from "./shell/ToastStack.jsx";
import ContextRail from "./shell/ContextRail.jsx";
import FirmwareUpdateNavItem from "./shell/FirmwareUpdateNavItem.jsx";
import {
  acknowledgeGenerationTask,
  subscribeGenerationTask,
} from "./lib/generation-task.js";
import petManagerMark from "./assets/logo/pet-manager-mark.svg";
import { version as clientVersion } from "../package.json";
import { initializeProviderConfigs } from "./lib/avatar-pipeline/provider-config.js";
import { loadDesktopStartup } from "./lib/desktop-startup.js";
import { installBrowserExitProtection, setExitReason, useExitReasons } from "./lib/exit-protection.js";

const DEV_DIRECT_DASHBOARD_BINDING = {
  boardDeviceId: "board-dev-direct-001",
  desktopDeviceId: "desktop-dev-direct-001",
  wifiSsid: "开发直达",
};

function devDirectDashboardBinding() {
  return import.meta.env.DEV && !hasTauriRuntime() ? DEV_DIRECT_DASHBOARD_BINDING : null;
}

function hasTauriRuntime() {
  return typeof window !== "undefined" && Boolean(window.__TAURI_INTERNALS__);
}

export default function App() {
  useEffect(installBrowserExitProtection, []);
  const [view, setView] = useState("loading"); // loading | dashboard | setup | gallery | wizard | detail | components | home | api
  const [binding, setBinding] = useState(null);
  const [detailAppearanceId, setDetailAppearanceId] = useState("");
  const [apiReturnView, setApiReturnView] = useState("");
  const [startupError, setStartupError] = useState("");
  const [configReadError, setConfigReadError] = useState("");
  const [startupAttempt, setStartupAttempt] = useState(0);

  const isPreviewBinding = useCallback((item) => {
    const boardId = String(item?.boardDeviceId || "").trim().toLowerCase();
    return boardId.includes("preview")
      || boardId === "board-ethernet-preview-001"
      || boardId === "board-preview-001";
  }, []);

  const enterBestAvailableDeviceSurface = useCallback((bindings) => {
    const stableBindings = Array.isArray(bindings)
      ? bindings.filter((item) => String(item?.boardDeviceId || "").trim() && !isPreviewBinding(item))
      : [];
    const devBinding = devDirectDashboardBinding();
    if (stableBindings.length > 0) {
      setBinding(stableBindings[stableBindings.length - 1]);
      setView("dashboard");
    } else if (devBinding) {
      setBinding(devBinding);
      setView("dashboard");
    } else {
      setBinding(null);
      setView("setup");
    }
  }, [isPreviewBinding]);

  useEffect(() => {
    let current = true;
    setStartupError("");
    loadDesktopStartup(initializeProviderConfigs, () => invoke("load_device_bindings"))
      .then(({bindings, configError}) => {
        if (!current) return;
        setConfigReadError(configError);
        enterBestAvailableDeviceSurface(bindings);
      })
      .catch(error => {
        if (!current) return;
        if (!hasTauriRuntime()) enterBestAvailableDeviceSurface([]);
        else setStartupError(error?.message || String(error));
      });
    return () => { current = false; };
  }, [enterBestAvailableDeviceSurface, startupAttempt]);

  const handleSetupComplete = useCallback(async () => {
    const bindings = await invoke("load_device_bindings");
    if (!Array.isArray(bindings) || !bindings.some(item =>
      String(item?.boardDeviceId || "").trim() && !isPreviewBinding(item))) {
      throw new Error("设备绑定尚未读回，请重新扫描。");
    }
    enterBestAvailableDeviceSurface(bindings);
  }, [enterBestAvailableDeviceSurface, isPreviewBinding]);

  const handleUnbind = useCallback(() => {
    setBinding(null);
    setView("setup");
  }, []);

  const handleOpenGallery = useCallback(() => {
    setDetailAppearanceId("");
    setView("gallery");
  }, []);

  const handleOpenComponents = useCallback(() => {
    setDetailAppearanceId("");
    setView("components");
  }, []);

  const handleEnterWizard = useCallback(() => setView("wizard"), []);

  const handleWizardExit = useCallback(() => setView("gallery"), []);

  const handleOpenDetail = useCallback((appearanceId) => {
    setDetailAppearanceId(appearanceId);
    setView("detail");
  }, []);

  const handleDetailBack = useCallback(() => {
    setDetailAppearanceId("");
    setView("gallery");
  }, []);

  const handleOpenApiSettings = useCallback(() => {
    setApiReturnView(view === "api" ? "" : view);
    setView("api");
  }, [view]);

  const handleApiSettingsBack = useCallback(() => {
    setView(apiReturnView && apiReturnView !== "api"
      ? apiReturnView
      : binding
        ? "dashboard"
        : "gallery");
    setApiReturnView("");
  }, [apiReturnView, binding]);

  const isDashboard = view === "dashboard";
  const isSetup = view === "setup";
  const hasBinding = Boolean(binding);
  const galleryViews = new Set(["gallery", "wizard", "detail"]);
  const activeTab = view === "api"
    ? "api"
    : view === "home" ? "home" : view === "components"
      ? "components"
      : galleryViews.has(view)
        ? "gallery"
        : "device";

  if (view === "loading") {
    return (
      <div className="app-shell">
        <div className="auth-shell">
          <div className="auth-card">
            <div className="auth-loading">
              {startupError ? <div role="alert"><p>读取本机配置失败，原数据未清空。</p><p>{startupError}</p>
                <button className="btn-primary" onClick={() => setStartupAttempt(n => n + 1)}>重试</button></div>
                : <><Loader2 size={18} className="spin" /><span>正在打开管理端…</span></>}
            </div>
          </div>
        </div>
      </div>
    );
  }

  return (
    <ToastProvider>
      <DeviceContextProvider
        binding={binding}
        onBindingChange={(next) => setBinding(next)}
      >
        <AppInner
          configReadError={configReadError}
          view={view}
          binding={binding}
          apiReturnView={apiReturnView}
          activeTab={activeTab}
          isDashboard={isDashboard}
          isSetup={isSetup}
          hasBinding={hasBinding}
          setView={setView}
          detailAppearanceId={detailAppearanceId}
          handleSetupComplete={handleSetupComplete}
          handleUnbind={handleUnbind}
          handleOpenGallery={handleOpenGallery}
          handleOpenComponents={handleOpenComponents}
          handleOpenApiSettings={handleOpenApiSettings}
          handleApiSettingsBack={handleApiSettingsBack}
          handleEnterWizard={handleEnterWizard}
          handleWizardExit={handleWizardExit}
          handleOpenDetail={handleOpenDetail}
          handleDetailBack={handleDetailBack}
        />
        <ToastStack />
      </DeviceContextProvider>
    </ToastProvider>
  );
}

function AppInner({
  configReadError,
  view,
  binding,
  apiReturnView,
  activeTab,
  isDashboard,
  isSetup,
  hasBinding,
  setView,
  detailAppearanceId,
  handleSetupComplete,
  handleUnbind,
  handleOpenGallery,
  handleOpenComponents,
  handleOpenApiSettings,
  handleApiSettingsBack,
  handleEnterWizard,
  handleWizardExit,
  handleOpenDetail,
  handleDetailBack,
}) {
  const { push, dismiss } = useToast();
  const activity = useExitReasons();
  const { refresh, usb } = useDeviceContext();
  const lastEpochRef = useRef(0);
  const generationToastRef = useRef(null);
  const [componentsVisited, setComponentsVisited] = useState(view === "components");
  useEffect(() => { if (view === "components") setComponentsVisited(true); }, [view]);
  const [apiVisited, setApiVisited] = useState(view === "api");
  useEffect(() => { if (view === "api") setApiVisited(true); }, [view]);

  const handleSetupCompleteWithRefresh = useCallback(async () => {
    await refresh();
    await handleSetupComplete();
  }, [handleSetupComplete, refresh]);

  useEffect(() => {
    return subscribeGenerationTask((s) => {
      setExitReason("avatar-generation",s.status === "running" ? "形象生成尚未完成" : "");
      // Closing the gallery result or starting another run also removes its
      // persistent toast. Clear the ref first: dismiss can notify this listener.
      if (s.status !== "completed" && s.status !== "failed") {
        const id = generationToastRef.current;
        generationToastRef.current = null;
        if (id !== null) dismiss(id);
        return;
      }
      if (s.completionEpoch <= lastEpochRef.current) return;
      lastEpochRef.current = s.completionEpoch;
      if (s.status === "completed") {
        refresh().catch((err) => {
          console.warn("[App] refresh after avatar generation failed", err);
        });
      }
      generationToastRef.current = push({
        tone: s.status === "completed" ? "success" : "error",
        title:
          s.status === "completed"
            ? `「${s.appearanceName}」${s.progress?.referenceImageError || s.resumable ? "部分生成完成" : "生成完成"}`
            : `「${s.appearanceName}」${s.appearanceId ? "部分已保存，后续未完成" : "生成失败"}`,
        message: s.status === "failed" ? s.error : s.progress?.referenceImageError ? s.progress.message : "",
        ttl: 0,
        onDismiss: () => acknowledgeGenerationTask(s.completionEpoch),
        action: s.appearanceId
          ? {
              label: "查看",
              onClick: () => {
                acknowledgeGenerationTask(s.completionEpoch);
                handleOpenDetail(s.appearanceId);
              },
            }
          : null,
      });
    });
  }, [push, dismiss, handleOpenDetail, refresh]);

  return (
    <div className="app-shell">
      <div className="app-layout">
        <aside className="app-sidebar">
          <div className="sidebar-head">
            <div className="sidebar-brand">
              <span className="sidebar-brand__mark">
                <img src={petManagerMark} alt="" />
              </span>
              <span className="sidebar-brand__copy">
                <strong className="sidebar-brand-name">HachimiDock</strong>
                <span>桌宠管理端 · v{clientVersion}</span>
              </span>
            </div>
          </div>
          <nav className="sidebar-nav">
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "device" ? "is-active" : ""}`}
              onClick={() => setView(hasBinding ? "dashboard" : "setup")}
              title="设备"
            >
              <MonitorSmartphone size={16} />
              <span className="sidebar-nav-label">设备</span>
            </button>
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "gallery" ? "is-active" : ""}`}
              onClick={handleOpenGallery}
              title="形象画廊"
            >
              <ImagePlus size={16} />
              <span className="sidebar-nav-label">形象画廊</span>
            </button>
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "components" ? "is-active" : ""}`}
              onClick={handleOpenComponents}
              title="组件中心"
            >
              <Blocks size={16} />
              <span className="sidebar-nav-label">组件中心</span>
            </button>
            <button type="button" className={`sidebar-nav__item ${activeTab === "home" ? "is-active" : ""}`} onClick={() => setView("home")} title="智能家居">
              <Home size={16} /><span className="sidebar-nav-label">智能家居</span>
            </button>
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "api" ? "is-active" : ""}`}
              onClick={handleOpenApiSettings}
              title="API 配置"
            >
              <KeyRound size={16} />
              <span className="sidebar-nav-label">API 配置</span>
            </button>
            <FirmwareUpdateNavItem usb={usb} />
          </nav>
          {!!activity.length && <details className="app-activity"><summary>{activity.length} 项进行中或未保存</summary>
            <ul>{activity.map(item => <li key={item}>{item}</li>)}</ul>
          </details>}
          <div className="sidebar-spacer" />
          <ContextRail
            onOpenDevice={() => setView(hasBinding ? "dashboard" : "setup")}
            onOpenAppearance={handleOpenGallery}
            onOpenComponent={handleOpenComponents}
            onStartBinding={() => setView("setup")}
          />
        </aside>
        <section className="app-main">
          <main className="app-content">
            {configReadError && <div className="message-banner message-banner--warning" role="alert">
              <strong>已有 API 配置暂时无法读取，原文件已保留。</strong>
              <p>设备管理和本地组件仍可使用；依赖这些凭据的服务暂不可用。不会自动清空或覆盖原配置。</p>
              <p>{configReadError}</p>
            </div>}
            <Suspense fallback={<p role="status">正在载入页面…</p>}>
            {isSetup && <>
              <p role="status">连接设备后可同步内容；也可以先浏览组件、创建形象或配置 API。</p>
              <DeviceSetup onComplete={handleSetupCompleteWithRefresh} />
            </>}
            {binding && (
              <DeviceDashboard
                active={isDashboard}
                binding={binding}
                onSwitchToSetup={() => setView("setup")}
                onUnbind={handleUnbind}
                onOpenGallery={handleOpenGallery}
                onOpenDetail={handleOpenDetail}
                onOpenApiSettings={handleOpenApiSettings}
              />
            )}
            {(view === "gallery" || (view === "api" && apiReturnView === "gallery")) && (
              <div hidden={view !== "gallery"}>
              <AppearanceGallery
                binding={binding}
                onEnterWizard={handleEnterWizard}
                onOpenDetail={handleOpenDetail}
                onOpenApiSettings={handleOpenApiSettings}
              />
              </div>
            )}
            {(view === "wizard" || (view === "api" && apiReturnView === "wizard")) && (
              <div hidden={view !== "wizard"}>
                <CustomAvatarWizard
                  onExit={handleWizardExit}
                  onOpenApiSettings={handleOpenApiSettings}
                />
              </div>
            )}
            {(view === "detail" || (view === "api" && apiReturnView === "detail"))
              && detailAppearanceId && (
                <div hidden={view !== "detail"}>
                  <AppearanceDetail
                    appearanceId={detailAppearanceId}
                    boardDeviceId={binding?.boardDeviceId || ""}
                    onBack={handleDetailBack}
                    onOpenApiSettings={handleOpenApiSettings}
                  />
                </div>
            )}
            {(componentsVisited || view === "components") && (
              <div hidden={view !== "components"}>
                <ComponentCenterBoundary><ComponentCenter active={view === "components"} /></ComponentCenterBoundary>
              </div>
            )}
            {(apiVisited || view === "api") && (
              <div hidden={view !== "api"}>
                <ApiSettings onBack={handleApiSettingsBack} configReadError={configReadError} />
              </div>
            )}
            {view === "home" && <SmartHome />}
            </Suspense>
          </main>
        </section>
      </div>
    </div>
  );
}
