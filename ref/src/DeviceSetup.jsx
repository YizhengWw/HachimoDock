/**
 * [Input] Device setup onboarding requirements and local Tauri/device bridge commands.
 * [Output] Two-method binding wizard with Ethernet mock fallback, dev skip-provisioning, connection-mode-aware retry/error copy, Wi-Fi provisioning, bridge verification, and final single-channel agent appearance confirmation.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useReducer, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  Wifi,
  EthernetPort,
  Cable,
  Signal,
  Loader,
  CheckCircle,
  XCircle,
  RotateCcw,
  ArrowRight,
  Eye,
  EyeOff,
  Code,
  Terminal,
  Zap,
} from "lucide-react";
import AppearancePreview from "./AppearancePreview.jsx";
import { listAppearances } from "./lib/appearance-store.js";
import { resolveGalleryPreviewMedia } from "./lib/appearance-preview.js";
import { hasTauriRuntime } from "./lib/tauri-env.js";
import {
  assignAppearanceToAgent,
  assignedAgentIds,
  appearanceById,
  loadAgentAppearanceMap,
  normalizeDetectedAgents,
  pickFirstDetectedAgentId,
  saveAgentAppearanceMap,
  saveEnabledAgents,
} from "./lib/agent-appearance-config.js";

const ETHERNET_BINDING_LABEL = "USB直连";
const ETHERNET_PREVIEW_DEVICE = {
  boardDeviceId: "board-ethernet-preview-001",
  pairingState: "usb_preview_ready",
  pairingMode: "USB (Preview)",
  portName: "preview-usb-port",
};
const SKIP_PROVISIONING_DEVICE = {
  boardDeviceId: "board-skip-setup-001",
  pairingState: "skip_provisioning_ready",
  pairingMode: "跳过配网",
};
const SKIP_PROVISIONING_SSID = "跳过配网";
const PREVIEW_DESKTOP_DEVICE_ID = "desktop-preview-001";
const NETWORK_NAMESPACE = "desk";

const STEPS = [
  { key: "connect_pet", label: "选择方式", desc: "网线或 Wi-Fi" },
  { key: "choose_wifi", label: "网络绑定", desc: "检测或配网" },
  { key: "verify_bridge", label: "验证通信", desc: "确认 Bridge 在线" },
  { key: "confirm_appearance", label: "确认形象", desc: "agent 形象" },
];

const AGENT_ICONS = {
  "claude-code": Code,
  codex: Terminal,
  openclaw: Zap,
};

function stepIndex(phase) {
  if (phase === "idle") return 0;
  if (phase === "connecting_ap" || phase === "fetching_info" || phase === "scanning_wifi") return 0;
  if (
    phase === "wait_user_input" ||
    phase === "applying_config" ||
    phase === "polling_result" ||
    phase === "ethernet_detecting"
  ) return 1;
  if (phase === "ethernet_binding" || phase === "restoring_wifi") return 2;
  if (phase === "choose_agent_appearance" || phase === "completed") return 3;
  if (phase === "error") return 0;
  return 0;
}

const INITIAL_STATE = {
  phase: "idle",
  originalSsid: null,
  boardDeviceId: "",
  pairingState: null,
  wifiNetworks: [],
  selectedSsid: "",
  manualSsid: false,
  password: "",
  showPassword: false,
  connectionMode: "wifi",
  desktopDeviceId: "",
  pollCount: 0,
  lastAttempt: null,
  resultIp: "",
  mqttVerified: null, // null = not checked, true = online, false = timeout
  testSent: null, // null = not sent, true = sent ok, false = failed
  testMessage: "",
  agents: [],
  agentScanLoading: false,
  agentScanError: "",
  appearances: [],
  appearanceLoadError: "",
  agentAppearanceDrafts: {},
  savingAgentAppearance: false,
  error: null,
  message: "",
};

function reducer(state, action) {
  switch (action.type) {
    case "set_phase":
      return { ...state, phase: action.phase, error: null, message: action.message || "" };
    case "set_connection_mode":
      return { ...state, connectionMode: action.value };
    case "set_original_ssid":
      return { ...state, originalSsid: action.ssid };
    case "set_device_info":
      return {
        ...state,
        boardDeviceId: action.boardDeviceId,
        pairingState: action.pairingState,
        desktopDeviceId: action.desktopDeviceId || state.desktopDeviceId,
      };
    case "set_wifi_networks":
      return { ...state, wifiNetworks: action.networks };
    case "set_selected_ssid":
      return { ...state, selectedSsid: action.ssid };
    case "set_manual_ssid":
      return { ...state, manualSsid: action.value };
    case "set_password":
      return { ...state, password: action.value };
    case "toggle_show_password":
      return { ...state, showPassword: !state.showPassword };
    case "set_poll_count":
      return { ...state, pollCount: action.count };
    case "set_last_attempt":
      return { ...state, lastAttempt: action.attempt };
    case "set_mqtt_verified":
      return { ...state, mqttVerified: action.value };
    case "set_test_result":
      return { ...state, testSent: action.ok, testMessage: action.message || "" };
    case "set_agent_scan_loading":
      return { ...state, agentScanLoading: action.value, agentScanError: action.value ? "" : state.agentScanError };
    case "set_agent_setup_data":
      return {
        ...state,
        agentScanLoading: false,
        agentScanError: "",
        appearanceLoadError: action.appearanceLoadError || "",
        agents: action.agents,
        appearances: action.appearances,
        agentAppearanceDrafts: action.agentAppearanceDrafts,
      };
    case "set_agent_scan_error":
      return { ...state, agentScanLoading: false, agentScanError: action.error || "" };
    case "set_agent_appearance_drafts":
      return { ...state, agentAppearanceDrafts: action.value };
    case "set_saving_agent_appearance":
      return { ...state, savingAgentAppearance: action.value };
    case "set_result":
      return {
        ...state,
        phase: "choose_agent_appearance",
        resultIp: action.ip || "",
        lastAttempt: action.attempt,
        connectionMode: action.connectionMode || state.connectionMode,
        error: null,
      };
    case "set_completed":
      return { ...state, phase: "completed", savingAgentAppearance: false, error: null, message: "" };
    case "set_error":
      return { ...state, phase: "error", error: action.error, message: action.message || "" };
    case "reset":
      return { ...INITIAL_STATE };
    default:
      return state;
  }
}

function StepBar({ currentPhase }) {
  const current = stepIndex(currentPhase);
  return (
    <div className="setup-steps">
      {STEPS.map((step, i) => {
        const isCompleted = currentPhase === "completed";
        const isDone = isCompleted ? i < STEPS.length : i < current;
        const isActive = isCompleted ? i === STEPS.length - 1 : i === current;
        let cls = "setup-steps__item";
        if (isDone) cls += " setup-steps__item--done";
        else if (isActive) cls += " setup-steps__item--active";
        return (
          <React.Fragment key={step.key}>
            <div className={cls}>
              <div className="setup-steps__dot">
                {isDone ? <CheckCircle size={14} /> : <span>{i + 1}</span>}
              </div>
              <div className="setup-steps__copy">
                <span className="setup-steps__label">{step.label}</span>
                <span className="setup-steps__desc">{step.desc}</span>
              </div>
            </div>
            {i < STEPS.length - 1 ? (
              <div className={`setup-steps__connector${isDone ? " is-done" : ""}`} />
            ) : null}
          </React.Fragment>
        );
      })}
    </div>
  );
}

function WifiStrengthBars({ signal }) {
  const strength = signal > -50 ? 4 : signal > -60 ? 3 : signal > -75 ? 2 : 1;

  return (
    <span className="wifi-bars" aria-label={`信号强度 ${strength}/4`}>
      {[1, 2, 3, 4].map((bar) => (
        <span
          key={bar}
          className={`wifi-bar ${bar <= strength ? "active" : ""}`}
          style={{ height: `${bar * 3 + 4}px` }}
        />
      ))}
    </span>
  );
}

function WizardCard({ eyebrow, title, description, footer, className = "", children }) {
  const classes = ["wizard-card", className].filter(Boolean).join(" ");

  return (
    <div className={classes}>
      <div className="wizard-card-head">
        <span className="wizard-card-eyebrow">{eyebrow}</span>
        <h3 className="wizard-card-title">{title}</h3>
        <p className="wizard-card-desc">{description}</p>
      </div>
      <div className="wizard-card-body">{children}</div>
      {footer ? <div className="wizard-card-foot">{footer}</div> : null}
    </div>
  );
}

export default function DeviceSetup({ onComplete } = {}) {
  const [state, dispatch] = useReducer(reducer, INITIAL_STATE);
  const abortRef = useRef(null);
  const canSkipProvisioning = import.meta.env.DEV;

  const abort = useCallback(() => {
    if (abortRef.current) {
      abortRef.current.aborted = true;
    }
  }, []);

  const loadAgentAppearanceSetup = useCallback(async () => {
    dispatch({ type: "set_agent_scan_loading", value: true });
    try {
      let appearanceLoadError = "";
      let appearances = [];
      try {
        appearances = await listAppearances();
      } catch (err) {
        appearanceLoadError = err?.message || String(err);
      }

      const response = hasTauriRuntime()
        ? await invoke("detect_local_agents")
        : {
            agents: [
              {
                id: "codex",
                label: "Codex",
                detected: true,
                detail: "预览模式检测到 Codex",
              },
            ],
          };
      const agents = normalizeDetectedAgents(response.agents || []);
      const detectedAgentIds = new Set(agents.filter((agent) => agent.detected).map((agent) => agent.id));
      const savedMap = loadAgentAppearanceMap(appearances);
      let agentAppearanceDrafts = Object.fromEntries(
        Object.entries(savedMap).filter(
          ([agentId, appearanceId]) =>
            detectedAgentIds.has(agentId) && Boolean(appearanceById(appearances, appearanceId)),
        ),
      );
      if (Object.keys(agentAppearanceDrafts).length === 0) {
        const defaultAgentId = pickFirstDetectedAgentId(agents);
        const defaultAppearanceId =
          Object.values(savedMap).find((appearanceId) => Boolean(appearanceById(appearances, appearanceId))) ||
          appearances[0]?.id ||
          "";
        if (defaultAgentId && defaultAppearanceId) {
          agentAppearanceDrafts = assignAppearanceToAgent({}, defaultAgentId, defaultAppearanceId);
        }
      }

      dispatch({
        type: "set_agent_setup_data",
        agents,
        appearances,
        appearanceLoadError,
        agentAppearanceDrafts,
      });
    } catch (err) {
      dispatch({ type: "set_agent_scan_error", error: err?.message || String(err) });
    }
  }, []);

  const syncSetupAgentAppearanceMapToBridge = useCallback(async (map) => {
    const enabledAgents = assignedAgentIds(map);
    saveEnabledAgents(new Set(enabledAgents));
    if (!hasTauriRuntime()) return;

    const profile = await invoke("load_bridge_profile");
    await invoke("save_bridge_profile", {
      input: {
        desktopDeviceId: profile.desktopDeviceId,
        mqttUrl: profile.mqttUrl,
        mqttNamespace: profile.mqttNamespace,
        mqttUsername: profile.mqttUsername,
        mqttPassword: profile.mqttPassword,
        petChannelId: profile.petChannelId,
        enabledAgents,
      },
    });
    await invoke("ensure_bridge_runtime", { input: { forceRestart: true } });
  }, []);

  const startSetup = useCallback(async () => {
    const ctx = { aborted: false };
    abortRef.current = ctx;
    dispatch({ type: "set_connection_mode", value: "wifi" });

    try {
      // Phase 1: Get current WiFi and connect to device AP
      dispatch({ type: "set_phase", phase: "connecting_ap", message: "正在记录当前网络..." });

      if (hasTauriRuntime()) {
        const wifiStatus = await invoke("wifi_get_status");
        if (ctx.aborted) return;
        dispatch({ type: "set_original_ssid", ssid: wifiStatus.currentSsid });

        dispatch({ type: "set_phase", phase: "connecting_ap", message: "正在连接设备热点 claw-pet，请稍候..." });
        const connectResult = await invoke("wifi_connect_ap");
        if (ctx.aborted) return;

        if (!connectResult.ok) {
          dispatch({ type: "set_error", error: connectResult.message, message: "连接设备热点失败。请确认设备已进入 AP 模式（热点名 claw-pet）。" });
          return;
        }
      } else {
        // Browser preview - skip WiFi connection
        dispatch({ type: "set_original_ssid", ssid: "preview-wifi" });
        await new Promise((r) => setTimeout(r, 800));
        if (ctx.aborted) return;
      }

      // Phase 2: Fetch device info
      dispatch({ type: "set_phase", phase: "fetching_info", message: "正在获取设备信息..." });

      let pairingState;
      if (hasTauriRuntime()) {
        pairingState = await invoke("device_get_pairing_state");
      } else {
        await new Promise((r) => setTimeout(r, 600));
        pairingState = {
          boardDeviceId: "board-preview-001",
          pairingState: "ap_fallback",
          pairingMode: "AP Fallback",
          apIp: "192.168.44.1",
          apSsid: "claw-pet",
        };
      }
      if (ctx.aborted) return;

      let desktopDeviceId = "";
      if (hasTauriRuntime()) {
        desktopDeviceId = await invoke("get_or_create_desktop_device_id");
      } else {
        desktopDeviceId = PREVIEW_DESKTOP_DEVICE_ID;
      }
      if (ctx.aborted) return;

      dispatch({
        type: "set_device_info",
        boardDeviceId: pairingState.boardDeviceId,
        pairingState: pairingState,
        desktopDeviceId,
      });

      // Phase 3: Scan WiFi
      dispatch({ type: "set_phase", phase: "scanning_wifi", message: "正在扫描周边 WiFi..." });

      let wifiScan;
      if (hasTauriRuntime()) {
        wifiScan = await invoke("device_get_wifi_scan");
      } else {
        await new Promise((r) => setTimeout(r, 500));
        wifiScan = {
          networks: [
            { ssid: "Home-WiFi-5G", signal: -45, secure: true },
            { ssid: "Office-Net", signal: -62, secure: true },
            { ssid: "Guest", signal: -78, secure: false },
          ],
          updatedAt: Date.now(),
        };
      }
      if (ctx.aborted) return;

      const networks = (wifiScan.networks || []).filter((n) => n.ssid);
      dispatch({ type: "set_wifi_networks", networks });

      if (networks.length === 0) {
        // Device scan returned empty — fall back to manual SSID input
        // Pre-fill with the WiFi the computer was on before switching to AP
        const fallbackSsid = state.originalSsid || "";
        dispatch({ type: "set_manual_ssid", value: true });
        dispatch({ type: "set_selected_ssid", ssid: fallbackSsid });
      } else {
        // Pre-select the original SSID if it's in the scan results
        const originalSsid = state.originalSsid;
        if (originalSsid && networks.some((n) => n.ssid === originalSsid)) {
          dispatch({ type: "set_selected_ssid", ssid: originalSsid });
        } else {
          dispatch({ type: "set_selected_ssid", ssid: networks[0].ssid });
        }
      }

      dispatch({ type: "set_phase", phase: "wait_user_input", message: "" });
    } catch (err) {
      if (!ctx.aborted) {
        dispatch({
          type: "set_error",
          error: String(err),
          message: "配网准备阶段失败",
        });
      }
    }
  }, [state.originalSsid]);

  const startEthernetBinding = useCallback(async () => {
    const ctx = { aborted: false };
    abortRef.current = ctx;
    dispatch({ type: "set_connection_mode", value: "ethernet" });

    try {
      dispatch({ type: "set_phase", phase: "ethernet_detecting", message: "正在检测 USB 设备..." });

      const desktopDeviceId = hasTauriRuntime()
        ? await invoke("get_or_create_desktop_device_id")
        : PREVIEW_DESKTOP_DEVICE_ID;
      if (ctx.aborted) return;

      let pairingState = ETHERNET_PREVIEW_DEVICE;
      let bindingBoardDeviceId = ETHERNET_PREVIEW_DEVICE.boardDeviceId;
      let bindingSsid = ETHERNET_BINDING_LABEL;

      if (hasTauriRuntime()) {
        const devices = await invoke("usb_scan_devices");
        if (ctx.aborted) return;
        if (!Array.isArray(devices) || devices.length === 0) {
          dispatch({
            type: "set_error",
            error: "未检测到可用 USB 串口设备",
            message: "请确认设备已开机并通过数据线连接电脑。",
          });
          return;
        }

        const preferred =
          devices.find((device) => Number(device?.vid) === 0x0525)
          || devices[0];
        const portName = String(preferred?.portName || "").trim();
        if (!portName) {
          dispatch({
            type: "set_error",
            error: "USB 端口信息无效",
            message: "请重新插拔设备后重试。",
          });
          return;
        }

        dispatch({
          type: "set_phase",
          phase: "ethernet_detecting",
          message: `检测到 ${portName}，正在建立 USB 会话...`,
        });
        await invoke("usb_connect", { portName });
        if (ctx.aborted) return;

        let usbStatus = null;
        for (let i = 0; i < 12; i += 1) {
          await new Promise((r) => setTimeout(r, 250));
          if (ctx.aborted) return;
          usbStatus = await invoke("usb_get_status");
          if (usbStatus?.connected && usbStatus?.boardDeviceId) {
            break;
          }
        }

        if (!usbStatus?.connected) {
          dispatch({
            type: "set_error",
            error: "USB 会话建立失败",
            message: "无法与设备建立 USB 通信，请检查线材或端口占用。",
          });
          return;
        }

        const boardDeviceId = String(usbStatus?.boardDeviceId || "").trim();
        const fallbackBoardDeviceId = `usb-${portName.replaceAll(/[^a-zA-Z0-9._-]/g, "-")}`;
        const resolvedBoardDeviceId = boardDeviceId || fallbackBoardDeviceId;

        pairingState = {
          boardDeviceId: resolvedBoardDeviceId,
          pairingState: "usb_ready",
          pairingMode: "USB",
          portName: usbStatus?.portName || portName,
        };
        bindingBoardDeviceId = resolvedBoardDeviceId;
        bindingSsid = `USB(${usbStatus?.portName || portName})`;
      } else {
        await new Promise((r) => setTimeout(r, 700));
        if (ctx.aborted) return;
      }

      dispatch({
        type: "set_device_info",
        boardDeviceId: bindingBoardDeviceId,
        pairingState,
        desktopDeviceId,
      });

      dispatch({ type: "set_phase", phase: "ethernet_binding", message: "正在写入绑定信息并验证连接..." });

      if (hasTauriRuntime()) {
        await invoke("save_device_binding", {
          binding: {
            boardDeviceId: bindingBoardDeviceId,
            desktopDeviceId,
            wifiSsid: bindingSsid,
            boundAt: Date.now(),
          },
        });

        try {
          await invoke("ensure_bridge_runtime", { input: null });
        } catch {
          // The bridge can already be running from a previous binding.
        }
      }
      if (ctx.aborted) return;

      await new Promise((r) => setTimeout(r, 900));
      if (ctx.aborted) return;

      dispatch({
        type: "set_result",
        ip: "",
        connectionMode: "ethernet",
        attempt: { ok: true, ssid: bindingSsid, connectionMode: "ethernet" },
      });
      await loadAgentAppearanceSetup();
    } catch (err) {
      if (!ctx.aborted) {
        dispatch({ type: "set_error", error: String(err), message: "插线绑定失败" });
      }
    }
  }, [loadAgentAppearanceSetup]);

  const skipProvisioning = useCallback(async () => {
    if (!canSkipProvisioning) return;
    const ctx = { aborted: false };
    abort();
    abortRef.current = ctx;
    dispatch({ type: "set_connection_mode", value: "skip" });

    try {
      const desktopDeviceId = hasTauriRuntime()
        ? await invoke("get_or_create_desktop_device_id")
        : PREVIEW_DESKTOP_DEVICE_ID;
      if (ctx.aborted) return;

      dispatch({
        type: "set_device_info",
        boardDeviceId: SKIP_PROVISIONING_DEVICE.boardDeviceId,
        pairingState: SKIP_PROVISIONING_DEVICE,
        desktopDeviceId,
      });

      if (hasTauriRuntime()) {
        await invoke("save_device_binding", {
          binding: {
            boardDeviceId: SKIP_PROVISIONING_DEVICE.boardDeviceId,
            desktopDeviceId,
            wifiSsid: SKIP_PROVISIONING_SSID,
            boundAt: Date.now(),
          },
        });

        try {
          await invoke("ensure_bridge_runtime", { input: null });
        } catch {
          // The bridge can already be running from a previous binding.
        }
      }
      if (ctx.aborted) return;

      dispatch({
        type: "set_result",
        ip: "",
        connectionMode: "skip",
        attempt: { ok: true, ssid: SKIP_PROVISIONING_SSID, connectionMode: "skip" },
      });
      await loadAgentAppearanceSetup();
    } catch (err) {
      if (!ctx.aborted) {
        dispatch({ type: "set_error", error: String(err), message: "跳过配网失败" });
      }
    }
  }, [abort, canSkipProvisioning, loadAgentAppearanceSetup]);

  const submitConfig = useCallback(async () => {
    const ctx = { aborted: false };
    abortRef.current = ctx;
    const ssid = state.manualSsid ? state.selectedSsid : state.selectedSsid;
    const password = state.password;

    if (!ssid) {
      dispatch({ type: "set_error", error: "请选择或输入 WiFi 名称", message: "" });
      return;
    }

    try {
      dispatch({ type: "set_phase", phase: "applying_config", message: `正在下发配置到设备...` });

      let applyResult;
      if (hasTauriRuntime()) {
        applyResult = await invoke("device_apply_config", {
          input: {
            ssid,
            password,
            desktopDeviceId: state.desktopDeviceId,
            mqttNamespace: NETWORK_NAMESPACE,
          },
        });
      } else {
        await new Promise((r) => setTimeout(r, 800));
        applyResult = { ok: true, pairingState: "sta_ready" };
      }
      if (ctx.aborted) return;

      if (!applyResult.ok) {
        dispatch({
          type: "set_error",
          error: applyResult.error || "配置下发失败",
          message: "设备拒绝了配置",
        });
        return;
      }

      // Phase: Restore WiFi immediately, then verify via MQTT
      dispatch({ type: "set_phase", phase: "restoring_wifi", message: "正在恢复电脑网络..." });

      if (hasTauriRuntime() && state.originalSsid) {
        try {
          await invoke("wifi_restore", { ssid: state.originalSsid, password: "" });
        } catch {
          // macOS will typically auto-reconnect to known networks
        }
      }
      if (ctx.aborted) return;

      // Save binding early — this also auto-creates bridge profile (pet-bridge.json)
      // which ensure_bridge_runtime needs to start the bridge process.
      if (hasTauriRuntime() && state.boardDeviceId) {
        try {
          await invoke("save_device_binding", {
            binding: {
              boardDeviceId: state.boardDeviceId,
              desktopDeviceId: state.desktopDeviceId,
              wifiSsid: ssid,
              boundAt: Date.now(),
            },
          });
        } catch {
          // Non-critical — bridge may still start from existing profile
        }
      }

      // Start bridge so it connects to MQTT and can receive device availability
      dispatch({ type: "set_phase", phase: "restoring_wifi", message: "正在启动通信服务..." });
      if (hasTauriRuntime()) {
        try {
          await invoke("ensure_bridge_runtime", { input: null });
        } catch {
          // Bridge may already be running
        }
      }
      if (ctx.aborted) return;

      // Wait a moment for bridge to connect to MQTT broker
      await new Promise((r) => setTimeout(r, 3000));
      if (ctx.aborted) return;

      // Poll device availability via MQTT
      dispatch({ type: "set_phase", phase: "polling_result", message: `正在等待设备通过网络上线...` });

      const MAX_MQTT_POLLS = 22; // 22 × 2s ≈ 44s
      const MQTT_POLL_INTERVAL = 2000;
      const boardId = state.boardDeviceId;

      for (let i = 0; i < MAX_MQTT_POLLS; i++) {
        if (ctx.aborted) return;
        dispatch({ type: "set_poll_count", count: i + 1 });

        await new Promise((r) => setTimeout(r, MQTT_POLL_INTERVAL));
        if (ctx.aborted) return;

        try {
          if (hasTauriRuntime()) {
            const availability = await invoke("check_device_availability");
            const deviceStatus = availability?.devices?.[boardId];
            if (deviceStatus?.online) {
              // Device is online via MQTT — pairing confirmed
              dispatch({
                type: "set_result",
                ip: "",
                attempt: { ok: true, ssid },
              });
              await loadAgentAppearanceSetup();
              return;
            }
          } else {
            // Browser preview — mock success on poll 5
            if (i >= 4) {
              dispatch({
                type: "set_result",
                ip: "192.168.1.100",
                attempt: { ok: true, ssid, ip: "192.168.1.100" },
              });
              await loadAgentAppearanceSetup();
              return;
            }
          }
        } catch {
          // Bridge not ready yet, keep polling
        }
      }

      dispatch({
        type: "set_error",
        error: "设备未能在 45 秒内通过网络上线，请检查 WiFi 密码是否正确",
        message: `连接 ${ssid} 超时`,
      });
    } catch (err) {
      if (!ctx.aborted) {
        dispatch({ type: "set_error", error: String(err), message: "配网失败" });
      }
    }
  }, [state, loadAgentAppearanceSetup]);

  const handleReset = useCallback(() => {
    abort();
    dispatch({ type: "reset" });
  }, [abort]);

  const handleRetry = useCallback(() => {
    const retryConnectionMode = state.connectionMode;
    const retryBinding = retryConnectionMode === "ethernet" ? startEthernetBinding : startSetup;
    abort();
    dispatch({ type: "reset" });
    setTimeout(() => retryBinding(), 100);
  }, [abort, startEthernetBinding, startSetup, state.connectionMode]);

  const sendTestMessage = useCallback(async () => {
    if (!hasTauriRuntime() || !state.desktopDeviceId) return;
    dispatch({ type: "set_test_result", ok: null, message: "发送中..." });
    try {
      const result = await invoke("send_test_message", {
        desktopDeviceId: state.desktopDeviceId,
        namespace: NETWORK_NAMESPACE,
        text: null,
      });
      if (result.ok) {
        dispatch({ type: "set_test_result", ok: true, message: "测试消息已发送，请查看设备屏幕" });
      } else {
        dispatch({ type: "set_test_result", ok: false, message: result.error || "发送失败" });
      }
    } catch (err) {
      dispatch({ type: "set_test_result", ok: false, message: String(err) });
    }
  }, [state.desktopDeviceId]);

  const selectSetupAgentChannel = useCallback((agentId) => {
    const currentAppearanceId =
      Object.values(state.agentAppearanceDrafts).find(Boolean) || state.appearances[0]?.id || "";
    dispatch({
      type: "set_agent_appearance_drafts",
      value: assignAppearanceToAgent({}, agentId, currentAppearanceId),
    });
  }, [state.agentAppearanceDrafts, state.appearances]);

  const selectSetupAgentAppearance = useCallback((appearanceId) => {
    const currentAgentId = Object.keys(state.agentAppearanceDrafts).find(Boolean) || pickFirstDetectedAgentId(state.agents);
    dispatch({
      type: "set_agent_appearance_drafts",
      value: assignAppearanceToAgent({}, currentAgentId, appearanceId),
    });
  }, [state.agentAppearanceDrafts, state.agents]);

  const completeAgentAppearanceSetup = useCallback(async () => {
    const detectedAgentIds = new Set(state.agents.filter((agent) => agent.detected).map((agent) => agent.id));
    const nextMap = Object.fromEntries(
      Object.entries(state.agentAppearanceDrafts).filter(
        ([agentId, appearanceId]) =>
          detectedAgentIds.has(agentId) && Boolean(appearanceById(state.appearances, appearanceId)),
      ),
    );

    if (assignedAgentIds(nextMap).length === 0) return;

    dispatch({ type: "set_saving_agent_appearance", value: true });
    try {
      saveAgentAppearanceMap(nextMap);
      await syncSetupAgentAppearanceMapToBridge(nextMap);
      dispatch({ type: "set_completed" });
    } catch (err) {
      dispatch({ type: "set_agent_scan_error", error: `形象配置保存失败：${err?.message || String(err)}` });
      dispatch({ type: "set_saving_agent_appearance", value: false });
    }
  }, [state.agents, state.agentAppearanceDrafts, state.appearances, syncSetupAgentAppearanceMapToBridge]);

  // Cleanup on unmount
  useEffect(() => abort, [abort]);

  const { phase } = state;
  const isPreview = !hasTauriRuntime();
  const currentHotspot = state.pairingState?.apSsid || "claw-pet";
  const selectedWifi = state.lastAttempt?.ssid || state.selectedSsid || "待选择";
  const countdown = Math.max(0, 44 - state.pollCount * 2);
  const isEthernetFlow = state.connectionMode === "ethernet";
  const isSkipFlow = state.connectionMode === "skip";
  const errorEyebrow = isEthernetFlow ? "插线绑定中断" : "配网中断";
  const errorTitle = state.message || (isEthernetFlow ? "插线绑定失败" : "配网失败");
  const errorDescription = isEthernetFlow ? "可重新检测 USB 或返回。" : "可重试或返回。";
  const completedConnectionLabel = isSkipFlow
    ? SKIP_PROVISIONING_SSID
    : isEthernetFlow
      ? ETHERNET_BINDING_LABEL
      : (state.lastAttempt?.ssid || selectedWifi);
  const detectedSetupAgents = state.agents.filter((agent) => agent.detected);
  const selectedSetupEntry = Object.entries(state.agentAppearanceDrafts).find(([, appearanceId]) =>
    Boolean(appearanceId),
  );
  const selectedSetupAgentId = selectedSetupEntry?.[0] || pickFirstDetectedAgentId(state.agents);
  const selectedSetupAgent = detectedSetupAgents.find((agent) => agent.id === selectedSetupAgentId);
  const selectedSetupAppearanceId = selectedSetupEntry?.[1] || state.appearances[0]?.id || "";
  const selectedSetupAppearance = appearanceById(state.appearances, selectedSetupAppearanceId);
  const selectedSetupAppearanceMedia = resolveGalleryPreviewMedia(selectedSetupAppearance);
  const canCompleteAgentAppearance = detectedSetupAgents.some((agent) =>
    Boolean(appearanceById(state.appearances, state.agentAppearanceDrafts[agent.id])),
  );

  return (
    <div className="setup setup--wizard">
      <StepBar currentPhase={phase} />

      {phase === "idle" && (
        <div className="connection-method-stack">
          <WizardCard
            className="wizard-card--wide wizard-card--compact setup-method-card setup-method-card--ethernet"
            eyebrow="方式一：插网线绑定"
            title="插网线绑定"
            description="插线后点击绑定。"
            footer={(
              <>
                <span className="wifi-list-label">插线 → 绑定</span>
                <button className="btn-primary" type="button" onClick={startEthernetBinding}>
                  <EthernetPort size={16} />
                  检测绑定
                </button>
              </>
            )}
          >
            <div className="method-summary">
              <div className="method-summary__icon">
                <Cable size={18} />
              </div>
              <div className="method-summary__copy">
                <span className="method-summary__label">直连</span>
                <span className="method-summary__value">无需密码</span>
              </div>
              <div className="method-steps" aria-label="插网线绑定步骤">
                <span><strong>1</strong>插线</span>
                <span><strong>2</strong>绑定</span>
              </div>
            </div>
          </WizardCard>

          <WizardCard
            className="wizard-card--wide setup-method-card"
            eyebrow="方式二：Wi‑Fi 配网"
            title="Wi‑Fi 绑定"
            description="连接热点，选择 Wi‑Fi。"
            footer={(
              <>
                <span className="wifi-list-label">AP 模式</span>
                {canSkipProvisioning && (
                  <button className="btn-ghost" type="button" onClick={skipProvisioning}>
                    跳过配网
                    <ArrowRight size={14} />
                  </button>
                )}
                <button className="btn-primary" type="button" onClick={startSetup}>
                  <Wifi size={16} />
                  开始配网
                </button>
              </>
            )}
          >
            <div className="hotspot-panel">
              <div className="hotspot-row">
                <div className="hotspot-icon">
                  <Wifi size={16} />
                </div>
                <div className="hotspot-meta">
                  <span className="hotspot-meta-label">热点</span>
                  <span className="hotspot-meta-value">{currentHotspot}</span>
                </div>
                <span className="hotspot-meta-hint">默认</span>
              </div>
              <div className="hotspot-row hotspot-row-secondary">
                <div className="hotspot-icon muted">
                  <Signal size={16} />
                </div>
                <div className="hotspot-meta">
                  <span className="hotspot-meta-label">电脑网络</span>
                  <span className="hotspot-meta-value">{state.originalSsid || "将在开始后自动记录"}</span>
                </div>
                <span className="hotspot-meta-hint">完成后恢复</span>
              </div>
              {isPreview ? (
                <div className="hotspot-warning">
                  <XCircle size={16} />
                  <span>预览模式：模拟数据。</span>
                </div>
              ) : (
                <div className="hotspot-status">
                  <Loader size={16} className="spin" />
                  <span>自动切网、扫描、恢复。</span>
                </div>
              )}
            </div>
          </WizardCard>
        </div>
      )}

      {(phase === "connecting_ap" || phase === "fetching_info" || phase === "scanning_wifi") && (
        <WizardCard
          eyebrow="第 1 步 / 共 4 步"
          title="正在连接桌宠"
          description="保持热点开启。"
        >
          <div className="hotspot-panel">
            <div className="hotspot-row">
              <div className="hotspot-icon">
                <Wifi size={16} />
              </div>
              <div className="hotspot-meta">
                <span className="hotspot-meta-label">热点</span>
                <span className="hotspot-meta-value">{currentHotspot}</span>
              </div>
              <span className="hotspot-meta-hint">连接中</span>
            </div>
            <div className="hotspot-row hotspot-row-secondary">
              <div className="hotspot-icon muted">
                <Signal size={16} />
              </div>
              <div className="hotspot-meta">
                <span className="hotspot-meta-label">电脑网络</span>
                <span className="hotspot-meta-value">{state.originalSsid || "正在记录当前网络..."}</span>
              </div>
              <span className="hotspot-meta-hint">稍后恢复</span>
            </div>
            <div className="hotspot-status">
              <Loader size={16} className="spin" />
              <span>{state.message}</span>
            </div>
            {(phase === "fetching_info" || phase === "scanning_wifi") && (
              <div className="hotspot-success">
                <CheckCircle size={16} />
                <span>已连接热点，正在扫描 Wi‑Fi。</span>
              </div>
            )}
          </div>
        </WizardCard>
      )}

      {phase === "wait_user_input" && (
        <WizardCard
          className="wizard-card--wide"
          eyebrow="第 2 步 / 共 4 步"
          title="选择 Wi‑Fi"
          description="选择网络。"
          footer={(
            <>
              <button className="btn-ghost" type="button" onClick={handleReset}>
                重新开始
              </button>
              <button
                className="btn-primary"
                type="button"
                onClick={submitConfig}
                disabled={!state.selectedSsid}
              >
                提交配网
                <ArrowRight size={14} />
              </button>
            </>
          )}
        >
          <div className="hotspot-panel">
            <div className="hotspot-row hotspot-row-secondary">
              <div className="hotspot-icon muted">
                <Wifi size={16} />
                </div>
                <div className="hotspot-meta">
                  <span className="hotspot-meta-label">热点</span>
                  <span className="hotspot-meta-value">{currentHotspot}</span>
                </div>
                <span className="hotspot-meta-hint">可下发配置</span>
              </div>
            {state.manualSsid ? (
              <div className="hotspot-warning">
                <XCircle size={16} />
                <span>手动输入 SSID。</span>
              </div>
            ) : (
              <>
                <div className="hotspot-status">
                  <Loader size={16} className="spin" />
                  <span>已扫描到 {state.wifiNetworks.length} 个 Wi‑Fi。</span>
                </div>
              </>
            )}
          </div>

          {!state.manualSsid ? (
            <div className="wifi-list">
              <div className="wifi-list-head">
                <span className="wifi-list-label">附近 Wi‑Fi</span>
                <span className="wifi-list-label">提交后连接</span>
              </div>
              <div className="wifi-list-items">
                {state.wifiNetworks.map((net) => {
                  const isSelected = state.selectedSsid === net.ssid;
                  return (
                    <label key={net.ssid} className={`wifi-item ${isSelected ? "selected" : ""}`}>
                      <input
                        type="radio"
                        name="wifi"
                        value={net.ssid}
                        checked={isSelected}
                        onChange={() => dispatch({ type: "set_selected_ssid", ssid: net.ssid })}
                      />
                      <span className="wifi-item-leading">
                        <Wifi size={14} />
                        <span className="wifi-item-name">{net.ssid}</span>
                        {net.secure ? <span className="wifi-item-secured">·加密</span> : <span className="wifi-item-secured">·开放</span>}
                      </span>
                      <span className="wifi-item-trailing">
                        <span className="wifi-item-signal">{net.signal} dBm</span>
                        <WifiStrengthBars signal={net.signal} />
                        {isSelected ? <CheckCircle size={15} className="wifi-item-check" /> : null}
                      </span>
                    </label>
                  );
                })}
                <button
                  className="wifi-list-more"
                  type="button"
                  onClick={() => dispatch({ type: "set_manual_ssid", value: true })}
                >
                  手动输入 SSID
                </button>
              </div>
            </div>
          ) : (
            <>
              <label className="field">
                <span className="field-label">Wi‑Fi 名称</span>
                <input
                  className="field-input"
                  type="text"
                  value={state.selectedSsid}
                  onChange={(e) => dispatch({ type: "set_selected_ssid", ssid: e.target.value })}
                  placeholder="输入 Wi‑Fi SSID"
                  autoFocus
                />
                <span className="field-helper">输入完整 SSID。</span>
              </label>
              <button
                className="btn-ghost"
                type="button"
                onClick={() => dispatch({ type: "set_manual_ssid", value: false })}
              >
                返回扫描列表
              </button>
            </>
          )}

          <label className="field">
            <span className="field-label">Wi‑Fi 密码</span>
            <div className="field-input-group">
              <input
                className="field-input"
                type={state.showPassword ? "text" : "password"}
                value={state.password}
                onChange={(e) => dispatch({ type: "set_password", value: e.target.value })}
                placeholder="输入 Wi‑Fi 密码（开放网络留空）"
              />
              <button
                className="field-input-toggle"
                type="button"
                onClick={() => dispatch({ type: "toggle_show_password" })}
                tabIndex={-1}
              >
                {state.showPassword ? <EyeOff size={16} /> : <Eye size={16} />}
              </button>
            </div>
            <span className="field-helper">本地使用，不上传。</span>
          </label>
        </WizardCard>
      )}

      {(phase === "ethernet_detecting" || phase === "ethernet_binding") && (
        <WizardCard
          eyebrow={phase === "ethernet_detecting" ? "第 2 步 / 共 4 步" : "第 3 步 / 共 4 步"}
          title={phase === "ethernet_detecting" ? "正在检测网线连接" : "正在完成插线绑定"}
          description="保持网线连接。"
          footer={(
            <>
              <span className="wifi-list-label">可返回重选</span>
              <button className="btn-ghost" type="button" onClick={handleReset}>
                返回开始页
              </button>
            </>
          )}
        >
          <div className="hotspot-panel">
            <div className="hotspot-row">
              <div className="hotspot-icon">
                <EthernetPort size={16} />
              </div>
              <div className="hotspot-meta">
                <span className="hotspot-meta-label">连接方式</span>
                <span className="hotspot-meta-value">{ETHERNET_BINDING_LABEL}</span>
              </div>
              <span className="hotspot-meta-hint">直连</span>
            </div>
            <div className="hotspot-status">
              <Loader size={16} className="spin" />
              <span>{state.message}</span>
            </div>
            {phase === "ethernet_binding" && (
              <div className="hotspot-success">
                <CheckCircle size={16} />
                <span>已检测到有线连接。</span>
              </div>
            )}
          </div>
        </WizardCard>
      )}

      {(phase === "applying_config" || phase === "polling_result" || phase === "restoring_wifi") && (
        <WizardCard
          eyebrow={phase === "applying_config" || phase === "polling_result" ? "第 2 步 / 共 4 步" : "第 3 步 / 共 4 步"}
          title="等待桌宠接入 Wi‑Fi"
          description="保持通电。"
        >
          <div className="hotspot-panel">
            <div className="hotspot-row">
              <div className="hotspot-icon">
                <Wifi size={16} />
              </div>
              <div className="hotspot-meta">
                <span className="hotspot-meta-label">目标 Wi‑Fi</span>
                <span className="hotspot-meta-value">{selectedWifi}</span>
              </div>
              <span className="hotspot-meta-hint">配置已下发</span>
            </div>
            {state.lastAttempt?.error && state.lastAttempt.error !== "pending" && (
              <div className="hotspot-warning">
                <XCircle size={16} />
                <span>{formatAttemptError(state.lastAttempt.error)}</span>
              </div>
            )}
            <div className="hotspot-status">
              <Loader size={16} className="spin" />
              <span>{state.message}</span>
            </div>
            {phase === "polling_result" && (
              <div className="hotspot-status">
                <Loader size={16} className="spin" />
                <span>等待上线（{state.pollCount}/22），约 {countdown} 秒。</span>
              </div>
            )}
            {phase === "restoring_wifi" && (
              <div className="hotspot-success">
                <CheckCircle size={16} />
                <span>正在切回 {state.originalSsid || "原网络"}。</span>
              </div>
            )}
          </div>
        </WizardCard>
      )}

      {phase === "choose_agent_appearance" && (
        <WizardCard
          className="wizard-card--wide wizard-card--agent-channel"
          eyebrow="第 4 步 / 共 4 步"
          title="选择设备展示渠道"
          description="设备端当前只能展示一个 agent 渠道。请选择桌宠要跟随的渠道，并确认默认形象。"
          footer={(
            <>
              <button
                className="btn-ghost"
                type="button"
                onClick={loadAgentAppearanceSetup}
                disabled={state.agentScanLoading || state.savingAgentAppearance}
              >
                {state.agentScanLoading ? <Loader size={14} className="spin" /> : <RotateCcw size={14} />}
                重新检测
              </button>
              <button
                className="btn-primary"
                type="button"
                onClick={completeAgentAppearanceSetup}
                disabled={!canCompleteAgentAppearance || state.agentScanLoading || state.savingAgentAppearance}
              >
                {state.savingAgentAppearance ? <Loader size={14} className="spin" /> : <CheckCircle size={14} />}
                完成绑定
              </button>
            </>
          )}
        >
          <div className="hotspot-panel">
            <div className="hotspot-success">
              <CheckCircle size={16} />
              <span>设备已连接：{completedConnectionLabel}</span>
            </div>
            {state.appearanceLoadError && (
              <div className="hotspot-warning">
                <XCircle size={16} />
                <span>读取本地形象失败，已保留内置形象入口：{state.appearanceLoadError}</span>
              </div>
            )}
            {state.agentScanError && (
              <div className="hotspot-warning">
                <XCircle size={16} />
                <span>{state.agentScanError}</span>
              </div>
            )}
          </div>

          {state.agentScanLoading ? (
            <div className="setup-agent-empty">
              <Loader size={18} className="spin" />
              <div>
                <strong>正在检测 CLI agent</strong>
                <span>检测完成后选择设备要跟随的渠道。</span>
              </div>
            </div>
          ) : detectedSetupAgents.length === 0 ? (
            <div className="setup-agent-empty">
              <XCircle size={18} />
              <div>
                <strong>未检测到可用 CLI agent</strong>
                <span>确认本机已安装并运行对应 CLI 后重新检测。</span>
              </div>
            </div>
          ) : (
            <div className="setup-agent-channel-layout">
              <div className="setup-agent-rule">
                <div>
                  <strong>当前展示：{selectedSetupAgent?.label || "已检测渠道"}</strong>
                  <span>
                    默认形象为 {selectedSetupAppearance ? selectedSetupAppearance.name : "内置形象"}，完成绑定后可在设备页调整。
                  </span>
                </div>
              </div>

              <div className="setup-agent-channel-grid">
                <div className="setup-agent-channel-list" role="radiogroup" aria-label="选择设备展示渠道">
                  {detectedSetupAgents.map((agent) => {
                    const Icon = AGENT_ICONS[agent.id] || Code;
                    const isSelected = agent.id === selectedSetupAgentId;
                    return (
                      <button
                        className={`setup-agent-channel-card${isSelected ? " is-selected" : ""}`}
                        key={agent.id}
                        type="button"
                        role="radio"
                        aria-checked={isSelected}
                        onClick={() => selectSetupAgentChannel(agent.id)}
                      >
                        <span className="setup-agent-channel-card__icon">
                          <Icon size={18} />
                        </span>
                        <span className="setup-agent-channel-card__copy">
                          <strong>{agent.label}</strong>
                          <small>{agent.detail}</small>
                        </span>
                        <span className="setup-agent-channel-card__state">
                          {isSelected ? "设备展示" : "可选择"}
                        </span>
                      </button>
                    );
                  })}
                </div>

                <section className="setup-agent-default-panel" aria-label="默认形象预览">
                  <div className="setup-agent-default-panel__preview">
                    {selectedSetupAppearance ? (
                      <AppearancePreview
                        media={selectedSetupAppearanceMedia}
                        className="appearance-channel-preview__media"
                        emptyClassName="appearance-channel-preview__empty"
                        playing={false}
                      />
                    ) : (
                      <div className="appearance-channel-preview__empty">未找到形象</div>
                    )}
                  </div>
                  <div className="setup-agent-default-panel__copy">
                    <span>默认形象</span>
                    <h4>{selectedSetupAppearance ? selectedSetupAppearance.name : "未找到可用形象"}</h4>
                    <p>{selectedSetupAppearance?.description || "读取本地形象后会在这里展示默认桌宠。"}</p>
                  </div>
                  {state.appearances.length > 0 && (
                    <label className="setup-agent-appearance-select">
                      <span>默认形象</span>
                      <select
                        value={selectedSetupAppearanceId}
                        onChange={(event) => selectSetupAgentAppearance(event.target.value)}
                      >
                        {state.appearances.map((row) => (
                          <option key={row.id} value={row.id}>
                            {row.name}
                          </option>
                        ))}
                      </select>
                    </label>
                  )}
                </section>
              </div>
            </div>
          )}
        </WizardCard>
      )}

      {phase === "completed" && (
        <WizardCard
          eyebrow="已完成"
          title="桌宠已完成绑定"
          description="设备展示渠道和默认形象已确认。"
          footer={(
            <>
              <button className="btn-ghost" type="button" onClick={sendTestMessage}>
                <Signal size={14} />
                发送测试消息
              </button>
              <button className="btn-primary" type="button" onClick={() => onComplete && onComplete()}>
                <ArrowRight size={14} />
                返回主界面
              </button>
            </>
          )}
        >
          <div className="hotspot-panel">
            <div className="hotspot-success">
              <CheckCircle size={16} />
              <span>已连接：{completedConnectionLabel}</span>
            </div>
            {state.testSent !== null && (
              <div className={state.testSent === true ? "hotspot-success" : "hotspot-warning"}>
                {state.testSent === true ? <CheckCircle size={16} /> : <XCircle size={16} />}
                <span>{state.testMessage}</span>
              </div>
            )}
          </div>
          <div className="setup-result">
            <div className="setup-result__row">
              <span>设备</span>
              <strong>{state.boardDeviceId}</strong>
            </div>
            <div className="setup-result__row">
              <span>连接方式</span>
              <strong>{completedConnectionLabel}</strong>
            </div>
            {state.resultIp && (
              <div className="setup-result__row">
                <span>IP 地址</span>
                <strong>{state.resultIp}</strong>
              </div>
            )}
          </div>
        </WizardCard>
      )}

      {phase === "error" && (
        <WizardCard
          eyebrow={errorEyebrow}
          title={errorTitle}
          description={errorDescription}
          footer={(
            <>
              <button className="btn-ghost" type="button" onClick={handleReset}>
                返回开始页
              </button>
              <button className="btn-primary" type="button" onClick={handleRetry}>
                <RotateCcw size={14} />
                重试
              </button>
            </>
          )}
        >
          <div className="hotspot-panel">
            <div className="hotspot-warning">
              <XCircle size={16} />
              <span>{state.error}</span>
            </div>
            {state.selectedSsid && (
              <div className="hotspot-row hotspot-row-secondary">
                <div className="hotspot-icon muted">
                  <Wifi size={16} />
                </div>
                <div className="hotspot-meta">
                  <span className="hotspot-meta-label">上次目标 Wi‑Fi</span>
                  <span className="hotspot-meta-value">{state.selectedSsid}</span>
                </div>
                <span className="hotspot-meta-hint">桌宠热点 {currentHotspot}</span>
              </div>
            )}
          </div>
        </WizardCard>
      )}
    </div>
  );
}

function formatAttemptError(error) {
  switch (error) {
    case "ssid_not_found":
      return "未找到指定的 WiFi 网络，请确认名称正确且设备在范围内";
    case "wrong_password_or_assoc":
      return "密码错误或无法关联到网络";
    case "no_dhcp_lease":
      return "已连接但未获取到 IP 地址（DHCP 失败）";
    default:
      return error;
  }
}
