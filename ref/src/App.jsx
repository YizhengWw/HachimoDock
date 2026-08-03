/**
 * [Input] Consume DeviceSetup.jsx and DeviceDashboard.jsx.
 * [Output] Pet Manager desktop app shell with first-level device/gallery/component-center/API-configuration sidebar tabs, a mounted-while-bound device dashboard that keeps Session/device synchronization alive across tabs, browser-only dev direct-dashboard fallback, USB-only first-run routing, setup-completion and avatar-generation DeviceContext refreshes, preserved generation state while API settings is open, and binding-scoped appearance management/downlink routing.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useState, useEffect, useCallback, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  ImagePlus,
  Loader2,
  MonitorSmartphone,
  Blocks,
  KeyRound,
} from "lucide-react";
import DeviceSetup from "./DeviceSetup";
import DeviceDashboard from "./DeviceDashboard";
import AppearanceGallery from "./AppearanceGallery";
import CustomAvatarWizard from "./CustomAvatarWizard";
import AppearanceDetail from "./AppearanceDetail";
import ComponentCenter from "./ComponentCenter";
import ApiSettings from "./ApiSettings";
import { DeviceContextProvider, useDeviceContext } from "./shell/DeviceContext.jsx";
import ToastStack, { ToastProvider, useToast } from "./shell/ToastStack.jsx";
import ContextRail from "./shell/ContextRail.jsx";
import {
  acknowledgeGenerationTask,
  subscribeGenerationTask,
} from "./lib/generation-task.js";
import petManagerMark from "./assets/logo/pet-manager-mark.svg";

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
  const [view, setView] = useState("loading"); // loading | dashboard | setup | gallery | wizard | detail | components | api
  const [binding, setBinding] = useState(null);
  const [detailAppearanceId, setDetailAppearanceId] = useState("");
  const [apiReturnView, setApiReturnView] = useState("");

  const isPreviewBinding = useCallback((item) => {
    const boardId = String(item?.boardDeviceId || "").trim().toLowerCase();
    return boardId.includes("preview")
      || boardId === "board-ethernet-preview-001"
      || boardId === "board-preview-001";
  }, []);

  const enterBestAvailableDeviceSurface = useCallback((bindings) => {
    const stableBindings = Array.isArray(bindings)
      ? bindings.filter((item) => !isPreviewBinding(item))
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
    invoke("load_device_bindings")
      .then(enterBestAvailableDeviceSurface)
      .catch(() => enterBestAvailableDeviceSurface([]));
  }, [enterBestAvailableDeviceSurface]);

  const handleSetupComplete = useCallback(async () => {
    try {
      const bindings = await invoke("load_device_bindings");
      enterBestAvailableDeviceSurface(bindings);
    } catch {
      // DeviceSetup keeps polling while a completed binding cannot be reloaded.
    }
  }, [enterBestAvailableDeviceSurface]);

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
    : view === "components"
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
              <Loader2 size={18} className="spin" />
              <span>正在打开管理端…</span>
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
  const { push } = useToast();
  const { refresh } = useDeviceContext();
  const lastEpochRef = useRef(0);

  const handleSetupCompleteWithRefresh = useCallback(async () => {
    await refresh();
    handleSetupComplete();
  }, [handleSetupComplete, refresh]);

  useEffect(() => {
    return subscribeGenerationTask((s) => {
      if (s.completionEpoch <= lastEpochRef.current) return;
      if (s.status !== "completed" && s.status !== "failed") return;
      lastEpochRef.current = s.completionEpoch;
      if (s.status === "completed") {
        refresh().catch((err) => {
          console.warn("[App] refresh after avatar generation failed", err);
        });
      }
      push({
        tone: s.status === "completed" ? "success" : "error",
        title:
          s.status === "completed"
            ? `「${s.appearanceName}」生成完成`
            : `「${s.appearanceName}」生成失败`,
        message: s.status === "failed" ? s.error : "",
        ttl: 6000,
        action: s.appearanceId
          ? {
              label: "查看",
              onClick: () => {
                acknowledgeGenerationTask();
                handleOpenDetail(s.appearanceId);
              },
            }
          : null,
      });
    });
  }, [push, handleOpenDetail, refresh]);

  if (isSetup) {
    return (
      <div className="app-shell wizard-mode">
        <div className="wizard-page">
          <header className="wizard-header wizard-header--shell">
            <div className="wizard-header-leading">
              <div className="wizard-brand">
                <img src={petManagerMark} alt="" />
              </div>
              <div className="wizard-header-copy">
                <span className="wizard-title">连接桌宠</span>
                <span className="wizard-subtitle">连接 USB，Pet Manager 会自动识别设备。</span>
              </div>
            </div>
          </header>
          <div className="wizard-page-body">
            <DeviceSetup onComplete={handleSetupCompleteWithRefresh} />
          </div>
        </div>
      </div>
    );
  }

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
                <strong className="sidebar-brand-name">桌宠管理端</strong>
                <span>Pet Manager</span>
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
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "api" ? "is-active" : ""}`}
              onClick={handleOpenApiSettings}
              title="API 配置"
            >
              <KeyRound size={16} />
              <span className="sidebar-nav-label">API 配置</span>
            </button>
          </nav>
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
            {binding && (
              <div hidden={!isDashboard}>
                <DeviceDashboard
                  binding={binding}
                  onSwitchToSetup={() => setView("setup")}
                  onUnbind={handleUnbind}
                  onOpenGallery={handleOpenGallery}
                  onOpenDetail={handleOpenDetail}
                  onOpenApiSettings={handleOpenApiSettings}
                />
              </div>
            )}
            {view === "gallery" && (
              <AppearanceGallery
                binding={binding}
                onEnterWizard={handleEnterWizard}
                onOpenDetail={handleOpenDetail}
              />
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
            {view === "components" && (
              <ComponentCenter />
            )}
            {view === "api" && (
              <ApiSettings onBack={handleApiSettingsBack} />
            )}
          </main>
        </section>
      </div>
    </div>
  );
}
