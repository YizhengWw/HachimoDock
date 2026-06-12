/**
 * [Input] Bound device, useDeviceContext for state, Tauri voice/USB events, and useToast for notices.
 * [Output] Device dashboard composed of PageShell + 4 cards: 状态条 / 当前展示 / 按钮配置 / 语音助手(折叠), with full visible button-map USB OTA confirmed by the Tauri command's board ack and board voice action injection routed through the managed bridge.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useReducer, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import DeviceGuideModal from "./DeviceGuideModal.jsx";
import { DEVICE_GUIDE_SEEN_KEY } from "./lib/device-guide-content.js";
import PageShell from "./shell/PageShell.jsx";
import Card from "./shell/Card.jsx";
import { useDeviceContext } from "./shell/DeviceContext.jsx";
import { useToast } from "./shell/ToastStack.jsx";
import DeviceStatusBar from "./dashboard/DeviceStatusBar.jsx";
import ChannelMatrixCard from "./dashboard/ChannelMatrixCard.jsx";
import BoardButtonPanel from "./dashboard/BoardButtonPanel.jsx";
import VoiceAssistantPanel, { buildVoiceSummary } from "./dashboard/VoiceAssistantPanel.jsx";
import DashboardActionsMenu from "./dashboard/DashboardActionsMenu.jsx";
import WifiApplyModal from "./dashboard/WifiApplyModal";
import { ACTIVE_APPEARANCE_KEY } from "./lib/desktop-pet-assignment.js";
import { BUILTIN_TERRIER_APPEARANCE_ID } from "./lib/builtin-appearances.js";

// ---------- Voice config storage + constants (re-exported for BoardButtonPanel) ----------

export const VOICE_CONFIG_STORAGE_KEY = "pet-manager.board-voice-config";
export const DEFAULT_VOICE_CONFIG = { enabled: false, trigger: "encoder_button.hold" };

export const DEFAULT_BUTTON_ACTIONS = {
  encoder_button_short: "system_page",
  encoder_button: "system_reset",
  encoder_rotate: "volume_adjust",
};

export const VOICE_BUTTON_OPTIONS = [
  { id: "encoder_button.hold", label: "前方旋钮按压", detail: "启用后按住说话；旋钮旋转仍切屏。", event: "voice.ptt.encoder_button.hold" },
];

export const BUTTON_FUNCTION_OPTIONS = [
  { id: "voice_ptt", label: "语音按住说话", detail: "按住接入语音；同一时间只允许一个硬件按钮作为语音触发。" },
  { id: "system_page", label: "系统切页", detail: "保持 main / stats 页面切换，适合旋钮短按。" },
  { id: "system_reset", label: "系统重置", detail: "保留长按重启或重置配网等板端默认能力。" },
  { id: "volume_adjust", label: "音量调节", detail: "旋钮旋转调节系统总音量，屏幕顶部短暂显示音量条。切页可继续用屏幕滑动。" },
  { id: "disabled", label: "不绑定", detail: "下发 disabled，让新版板端忽略该输入；不会继续触发系统切页或负一屏操作。" },
];

export const BOARD_BUTTON_CONTROL_ROWS = [
  { id: "encoder_button_short", label: "前方旋钮短按", event: "button.encoder.short_press", defaultAction: "system_page", actionOptions: ["system_page", "disabled"] },
  { id: "encoder_button", label: "前方旋钮长按", event: "button.encoder.long_press", voiceTriggerId: "encoder_button.hold", defaultAction: "system_reset", actionOptions: ["voice_ptt", "system_reset", "disabled"] },
  { id: "encoder_rotate", label: "前方旋钮旋转", event: "knob.rotate_cw / knob.rotate_ccw", defaultAction: "volume_adjust", actionOptions: ["volume_adjust"] },
];

export function actionOptionById(actionId) {
  return BUTTON_FUNCTION_OPTIONS.find((option) => option.id === actionId) || BUTTON_FUNCTION_OPTIONS[0];
}

export function buildBoardButtonConfigBindings(buttonActions = {}) {
  return BOARD_BUTTON_CONTROL_ROWS.map((row) => ({
    event: row.otaEvent || row.event,
    action: row.actionOptions.includes(buttonActions[row.id]) ? buttonActions[row.id] : row.defaultAction,
  }));
}

function createButtonConfigRequestId() {
  return `button-config-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
}

function normalizeVoiceConfig(value = {}) {
  const triggerIds = new Set(VOICE_BUTTON_OPTIONS.map((o) => o.id));
  const trigger = triggerIds.has(value.trigger) ? value.trigger : DEFAULT_VOICE_CONFIG.trigger;
  const incoming = value.buttonActions && typeof value.buttonActions === "object" ? value.buttonActions : {};
  const buttonActions = BOARD_BUTTON_CONTROL_ROWS.reduce((next, row) => {
    next[row.id] = row.actionOptions.includes(incoming[row.id]) ? incoming[row.id] : DEFAULT_BUTTON_ACTIONS[row.id] || row.defaultAction;
    return next;
  }, {});
  return { enabled: value.enabled === true, trigger, buttonActions };
}

function loadVoiceConfigFromStorage() {
  try {
    const raw = localStorage.getItem(VOICE_CONFIG_STORAGE_KEY);
    if (raw) return normalizeVoiceConfig(JSON.parse(raw));
  } catch {}
  return normalizeVoiceConfig({});
}

function saveVoiceConfigToStorage(next) {
  try {
    localStorage.setItem(VOICE_CONFIG_STORAGE_KEY, JSON.stringify(normalizeVoiceConfig(next)));
  } catch {}
}

// Single place that OTAs a button config to the board. Used by both the device
// dashboard's manual "下发" button and the component center's install flow, so
// the invoke shape lives in exactly one spot.
export async function dispatchBoardButtonConfig({ boardDeviceId, buttonActions, voiceButton, voiceEnabled }) {
  const requestId = createButtonConfigRequestId();
  return invoke("button_config_signal", {
    boardDeviceId,
    requestId,
    bindings: buildBoardButtonConfigBindings(buttonActions),
    voiceButton,
    voiceEnabled,
  });
}

// ---------- Voice-bus helpers (unchanged from previous file) ----------
const VOICE_BUS_URL = "http://127.0.0.1:8181";
const BRIDGE_HTTP_PRIMARY_PORT = 23333;

function normalizeText(value) {
  return typeof value === "string" ? value.trim() : "";
}

async function fetchBusStatus(signal) {
  const resp = await fetch(`${VOICE_BUS_URL}/agent/status`, { signal });
  if (!resp.ok) throw new Error(`bus status http ${resp.status}`);
  const body = await resp.json();
  const list = Array.isArray(body?.adapters) ? body.adapters : Array.isArray(body?.agents) ? body.agents : [];
  return { ok: body?.ok !== false, agents: list };
}
async function fetchBusSessions(agentId, signal) {
  if (!agentId) return [];
  const resp = await fetch(`${VOICE_BUS_URL}/agent/sessions?agentId=${encodeURIComponent(agentId)}&limit=20`, { signal });
  if (!resp.ok) throw new Error(`bus sessions http ${resp.status}`);
  const body = await resp.json();
  return Array.isArray(body?.sessions) ? body.sessions : [];
}

async function postMockButtonInject(payload) {
  await invoke("ensure_bridge_runtime", { input: { forceRestart: false } }).catch(() => {});

  const ports = [];
  try {
    const runtime = await invoke("load_bridge_runtime_status");
    const runtimePort = Number(runtime?.port);
    if (Number.isFinite(runtimePort) && runtimePort > 0) ports.push(runtimePort);
  } catch {
    // Older desktop runtimes do not expose this command; use the managed bridge port below.
  }
  ports.push(BRIDGE_HTTP_PRIMARY_PORT);

  let lastError = null;
  for (const port of [...new Set(ports)]) {
    try {
      const url = `http://127.0.0.1:${port}/mock-button-inject`;
      const result = await invoke("http_request_text", {
        url,
        method: "POST",
        headersJson: JSON.stringify({
          "Content-Type": "application/json",
          Accept: "application/json",
        }),
        body: JSON.stringify(payload),
        timeoutMs: 120000,
      });
      const parsed = (() => {
        try {
          return JSON.parse(result?.body || "{}");
        } catch {
          return {};
        }
      })();
      if (!result?.ok || parsed?.ok === false) {
        throw new Error(parsed?.error || `mock-button-inject http ${result?.status || 0}`);
      }
      return parsed;
    } catch (err) {
      lastError = err;
    }
  }
  throw lastError || new Error("mock-button-inject unavailable");
}

// ---------- Voice reducer (lean — only what VoiceAssistantPanel needs) ----------
const VOICE_INITIAL_STATE = {
  busStatus: null,
  busSessions: null,
  busSessionId: "auto",
  voiceRuntime: null,
  audioBridgeEnabled: false,
  audioBridgePending: false,
  audioBridgeMessage: "",
  audioBridgeLastResult: null,
  mockInjectInput: "",
  mockInjectPending: false,
  mockInjectMessage: "",
  mockInjectReply: "",
  mockInjectOk: null,
  deviceVoiceFlow: {
    phase: "idle",
    text: "",
    message: "",
    reply: "",
    ok: null,
    agentId: "",
    sessionId: "",
    updatedAt: 0,
  },
};

function voiceReducer(state, action) {
  switch (action.type) {
    case "set_bus_status": return { ...state, busStatus: action.value };
    case "set_bus_sessions": return { ...state, busSessions: action.value };
    case "set_bus_session_id": return { ...state, busSessionId: action.value || "auto" };
    case "set_voice_runtime": return { ...state, voiceRuntime: action.value };
    case "set_mock_inject_input": return { ...state, mockInjectInput: action.value || "" };
    case "set_mock_inject_pending":
      return {
        ...state,
        mockInjectPending: Boolean(action.value),
        ...(action.value ? { mockInjectMessage: "", mockInjectReply: "", mockInjectOk: null } : {}),
      };
    case "set_mock_inject_result":
      return {
        ...state,
        mockInjectPending: false,
        mockInjectOk: action.ok,
        mockInjectMessage: action.message || "",
        mockInjectReply: action.reply || "",
      };
    case "set_device_voice_flow_progress":
      return {
        ...state,
        deviceVoiceFlow: {
          phase: action.phase || "injecting",
          text: action.text || state.deviceVoiceFlow.text || "",
          message: action.message || "设备语音识别完成，正在发送到当前会话...",
          reply: "",
          ok: null,
          agentId: action.agentId || state.deviceVoiceFlow.agentId || "",
          sessionId: action.sessionId || state.deviceVoiceFlow.sessionId || "",
          updatedAt: Date.now(),
        },
      };
    case "set_device_voice_flow_result":
      return {
        ...state,
        deviceVoiceFlow: {
          phase: action.ok ? "done" : "error",
          text: action.text || state.deviceVoiceFlow.text || "",
          message: action.message || (action.ok ? "已发送到当前会话" : "发送失败"),
          reply: action.reply || "",
          ok: action.ok === true,
          agentId: action.agentId || state.deviceVoiceFlow.agentId || "",
          sessionId: action.sessionId || state.deviceVoiceFlow.sessionId || "",
          updatedAt: Date.now(),
        },
      };
    case "set_audio_bridge_pending": return { ...state, audioBridgePending: action.value };
    case "set_audio_bridge_state":
      return {
        ...state,
        audioBridgeEnabled: action.enabled,
        audioBridgePending: false,
        audioBridgeLastResult: action.ok ? "ok" : "error",
        audioBridgeMessage: action.message || "",
      };
    default: return state;
  }
}

// ---------- Component ----------

export default function DeviceDashboard({ binding, onUnbind }) {
  const { usb, deviceOnline, onlineBoardDeviceId, currentDisplay } = useDeviceContext();
  const { push } = useToast();

  const [voiceState, voiceDispatch] = useReducer(voiceReducer, VOICE_INITIAL_STATE);
  const [voiceConfig, setVoiceConfig] = useState(loadVoiceConfigFromStorage);
  const [voiceConfigDirty, setVoiceConfigDirty] = useState(false);
  const [voiceConfigOtaState, setVoiceConfigOtaState] = useState({ pending: false, tone: "", message: "" });
  const [guideOpen, setGuideOpen] = useState(false);
  const [wifiModalOpen, setWifiModalOpen] = useState(false);

  // When another surface (e.g. the component center installing a widget) writes
  // a new button config to the shared store and fires a "storage" event, reload
  // it so this panel reflects the applied preset instead of going stale.
  useEffect(() => {
    const onStorage = () => {
      setVoiceConfig(loadVoiceConfigFromStorage());
      setVoiceConfigDirty(false);
    };
    window.addEventListener("storage", onStorage);
    return () => window.removeEventListener("storage", onStorage);
  }, []);

  // Auto-open the device-guide modal the first time the user lands here.
  useEffect(() => {
    if (!binding) return;
    try {
      if (!localStorage.getItem(DEVICE_GUIDE_SEEN_KEY)) setGuideOpen(true);
    } catch {}
  }, [binding]);

  const selectedAgentId = currentDisplay.agentId;
  const selectedBusAgent = Array.isArray(voiceState.busStatus?.agents)
    ? voiceState.busStatus.agents.find((agent) => agent.agentId === selectedAgentId)
    : null;
  const selectedBusAgentReady = selectedBusAgent?.ready === true;

  // Restore last-picked voice session per agent.
  useEffect(() => {
    if (!selectedAgentId) return;
    try {
      const raw = localStorage.getItem(`pet-manager.voice-session.${selectedAgentId}`);
      voiceDispatch({ type: "set_bus_session_id", value: raw || "auto" });
    } catch {
      voiceDispatch({ type: "set_bus_session_id", value: "auto" });
    }
  }, [selectedAgentId]);

  // Poll voice-bus status.
  useEffect(() => {
    let cancelled = false;
    const ctl = new AbortController();
    const run = () => {
      fetchBusStatus(ctl.signal)
        .then((body) => { if (!cancelled) voiceDispatch({ type: "set_bus_status", value: body }); })
        .catch(() => { if (!cancelled) voiceDispatch({ type: "set_bus_status", value: { ok: false, agents: [] } }); });
    };
    run();
    const id = setInterval(run, 5000);
    return () => { cancelled = true; ctl.abort(); clearInterval(id); };
  }, []);

  // Poll voice-runtime.
  useEffect(() => {
    let cancelled = false;
    const run = () => {
      invoke("ensure_voice_runtime")
        .then((res) => {
          if (cancelled) return;
          voiceDispatch({
            type: "set_voice_runtime",
            value: {
              mode: res?.mode || null,
              message: res?.message || "",
              running: !!res?.running,
              agentId: res?.selectedAgentId || res?.profile?.selectedAgentId || "",
            },
          });
        })
        .catch((err) => {
          if (cancelled) return;
          voiceDispatch({ type: "set_voice_runtime", value: { mode: "error", message: String(err), running: false, agentId: "" } });
        });
    };
    run();
    const id = setInterval(run, 5000);
    return () => { cancelled = true; clearInterval(id); };
  }, [selectedAgentId]);

  // Refetch sessions for the current agent. The packaged app can render this
  // panel before the managed bridge has finished booting; keep retrying once
  // the selected agent is ready so a cold-start miss does not leave the select
  // permanently empty/disabled.
  useEffect(() => {
    if (!selectedAgentId) { voiceDispatch({ type: "set_bus_sessions", value: [] }); return undefined; }
    if (!selectedBusAgentReady) return undefined;
    let cancelled = false;
    const ctl = new AbortController();
    const run = () => {
      fetchBusSessions(selectedAgentId, ctl.signal)
        .then((sessions) => { if (!cancelled) voiceDispatch({ type: "set_bus_sessions", value: sessions }); })
        .catch(() => { if (!cancelled) voiceDispatch({ type: "set_bus_sessions", value: [] }); });
    };
    run();
    const id = setInterval(run, 5000);
    return () => { cancelled = true; ctl.abort(); clearInterval(id); };
  }, [selectedAgentId, selectedBusAgentReady]);

  // Reflect board-originated voice_input action round trips in the voice panel.
  useEffect(() => {
    let disposed = false;
    let unlistenUsbMessage = null;
    let unlistenUsbResult = null;

    const setupListeners = async () => {
      unlistenUsbMessage = await listen("usb-message", (event) => {
        const envelope = event?.payload && typeof event.payload === "object" ? event.payload : {};
        if (envelope.topic !== "input/action") return;
        const payload = envelope.payload && typeof envelope.payload === "object" ? envelope.payload : {};
        const view = normalizeText(payload.view).toLowerCase();
        if (view !== "voice_input") return;
        const text = normalizeText(payload.state);
        if (!text) return;
        voiceDispatch({
          type: "set_device_voice_flow_progress",
          phase: "injecting",
          text,
          message: "设备语音识别完成，正在发送到当前会话...",
        });
      });

      unlistenUsbResult = await listen("usb-input-action-result", (event) => {
        const payload = event?.payload && typeof event.payload === "object" ? event.payload : {};
        const view = normalizeText(payload.view).toLowerCase();
        if (view && view !== "voice_input") return;

        const ok = payload.ok === true;
        const transient = payload.transient === true;
        const pending = payload.pending === true;
        const text = normalizeText(payload.text);
        const agentId = normalizeText(payload.agentId);
        const sessionId = normalizeText(payload.sessionId);
        const reply = normalizeText(
          payload.tokenPreview
          || payload.replyPreview
          || payload.response?.tokenPreview
        );
        const defaultSuccessMessage = `已发送到 ${agentId || "当前工具"} · 会话 ${sessionId || "auto"}`;
        const message = normalizeText(payload.message)
          || (!ok ? normalizeText(payload.error) : "")
          || (ok ? defaultSuccessMessage : "发送失败");

        if (pending) {
          voiceDispatch({
            type: "set_device_voice_flow_progress",
            phase: "waiting_reply",
            text,
            message: message || "已发送到当前会话，等待模型回复...",
            agentId,
            sessionId,
          });
          return;
        }

        if (!ok && transient) {
          voiceDispatch({
            type: "set_device_voice_flow_progress",
            phase: "injecting",
            text,
            message: `桥接连接瞬时抖动，正在确认是否已送达...\n${message}`,
            agentId,
            sessionId,
          });
          return;
        }

        voiceDispatch({
          type: "set_device_voice_flow_result",
          ok,
          text,
          message,
          reply,
          agentId,
          sessionId,
        });
      });

      if (disposed) {
        unlistenUsbMessage?.();
        unlistenUsbResult?.();
      }
    };

    setupListeners().catch((err) => {
      console.warn("[voice] failed to listen for USB voice action events", err);
    });

    return () => {
      disposed = true;
      unlistenUsbMessage?.();
      unlistenUsbResult?.();
    };
  }, []);

  // ---------- Button config update + OTA dispatch ----------
  const updateVoiceConfig = useCallback((patch) => {
    setVoiceConfig((prev) => {
      const next = normalizeVoiceConfig({ ...prev, ...patch });
      saveVoiceConfigToStorage(next);
      setVoiceConfigDirty(true);
      setVoiceConfigOtaState({ pending: false, tone: "warning", message: "已保存到客户端；按钮配置需要通过 USB OTA 下发到板端后才会生效。" });
      return next;
    });
  }, []);

  const applyVoiceConfigOverUsb = useCallback(async () => {
    if (!usb.connected) {
      setVoiceConfigOtaState({ pending: false, tone: "warning", message: "需要先通过 USB 连接设备，才能把按钮配置 OTA 到板端。" });
      return;
    }
    const targetBoardDeviceId = onlineBoardDeviceId || usb.boardDeviceId || binding.boardDeviceId;
    if (!targetBoardDeviceId) {
      setVoiceConfigOtaState({ pending: false, tone: "error", message: "未找到可用的板子 ID，请先完成设备绑定。" });
      return;
    }
    setVoiceConfigOtaState({ pending: true, tone: "", message: "正在通过 USB OTA 下发完整按钮配置到板端..." });
    try {
      const ack = await dispatchBoardButtonConfig({
        boardDeviceId: targetBoardDeviceId,
        buttonActions: voiceConfig.buttonActions,
        voiceButton: voiceConfig.trigger,
        voiceEnabled: voiceConfig.enabled,
      });
      const bindingCount = Number(ack?.bindingCount ?? ack?.ack?.bindingCount ?? 0);
      setVoiceConfigDirty(false);
      setVoiceConfigOtaState({
        pending: false,
        tone: "success",
        message: `按钮配置已写入板端（${Number.isFinite(bindingCount) ? bindingCount : 0} 项）；旋钮“不绑定”会在新版板端运行时立即停止切页。`,
      });
    } catch (err) {
      setVoiceConfigOtaState({ pending: false, tone: "error", message: `按钮配置下发失败: ${err}` });
    }
  }, [binding.boardDeviceId, onlineBoardDeviceId, usb.boardDeviceId, usb.connected, voiceConfig.buttonActions, voiceConfig.enabled, voiceConfig.trigger]);

  // ---------- Audio bridge toggle ----------
  const toggleAudioBridge = useCallback(async (action) => {
    const requestedEnabled = action === "start";
    const targetBoardDeviceId = onlineBoardDeviceId || usb.boardDeviceId || binding.boardDeviceId;
    voiceDispatch({ type: "set_audio_bridge_pending", value: true });
    try {
      if (!targetBoardDeviceId) throw new Error("未找到可用的板子 ID，请先完成设备绑定。");
      if (action === "start") {
        const bridgeRuntime = await invoke("ensure_bridge_runtime");
        if (bridgeRuntime?.running === false) throw new Error(bridgeRuntime?.message || "本地 Bridge 未启动，无法下发板子音频信令。");
        const voiceRuntime = await invoke("ensure_voice_runtime");
        if (!voiceRuntime?.running) throw new Error(voiceRuntime?.message || "voice-service 未启动，无法接入板子音频。");
      }
      const res = await invoke("audio_bridge_signal", {
        boardDeviceId: targetBoardDeviceId,
        action,
        voiceButton: voiceConfig.trigger,
      });
      const transports = [res?.usbSent ? "USB" : "", res?.mqttSent ? "MQTT" : ""].filter(Boolean).join(" / ");
      voiceDispatch({ type: "set_audio_bridge_state", enabled: requestedEnabled, ok: true, message: `已通过 ${transports || "USB / MQTT"} 下发到板端` });
    } catch (err) {
      voiceDispatch({ type: "set_audio_bridge_state", enabled: !requestedEnabled, ok: false, message: `${action === "start" ? "启动" : "关闭"}板子音频失败: ${err}` });
    }
  }, [binding.boardDeviceId, onlineBoardDeviceId, usb.boardDeviceId, voiceConfig.trigger]);

  const sendMockButtonInject = useCallback(() => {
    const text = (voiceState.mockInjectInput || "").trim();
    if (!text || !selectedAgentId) return;
    voiceDispatch({ type: "set_mock_inject_pending", value: true });
    postMockButtonInject({
      agentId: selectedAgentId,
      sessionId: voiceState.busSessionId || "auto",
      text,
    })
      .then((response) => {
        const sessionId =
          response?.sessionId
          || response?.done?.sessionId
          || response?.ready?.sessionId
          || voiceState.busSessionId
          || "auto";
        voiceDispatch({
          type: "set_mock_inject_result",
          ok: true,
          message: `已发送到当前会话 · ${sessionId}`,
          reply: response?.tokenPreview || "",
        });
      })
      .catch((err) => {
        voiceDispatch({
          type: "set_mock_inject_result",
          ok: false,
          message: err?.message || String(err),
          reply: "",
        });
      });
  }, [selectedAgentId, voiceState.busSessionId, voiceState.mockInjectInput]);

  // ---------- Action-menu callbacks ----------
  const onSendTest = useCallback(() => {
    const sendPromise = usb.connected
      ? invoke("usb_send_speech", { text: "hello from HachimoDock" }).then(() => ({ ok: true })).catch((err) => ({ ok: false, error: String(err) }))
      : invoke("send_test_message", { desktopDeviceId: binding.desktopDeviceId, namespace: null, text: null });
    sendPromise.then((res) => {
      push(res.ok
        ? { tone: "success", title: "测试消息已发送" }
        : { tone: "error", title: "测试消息发送失败", message: res.error });
    });
  }, [binding.desktopDeviceId, push, usb.connected]);

  const onCopyDesktopId = useCallback(() => {
    try {
      navigator.clipboard?.writeText(binding.desktopDeviceId || "");
      push({ tone: "success", title: "已复制桌面设备 ID" });
    } catch {
      push({ tone: "error", title: "复制失败" });
    }
  }, [binding.desktopDeviceId, push]);

  const onUnbindClick = useCallback(() => {
    invoke("remove_device_binding", { boardDeviceId: binding.boardDeviceId })
      .then(() => onUnbind?.())
      .catch((err) => push({ tone: "error", title: "解绑失败", message: String(err) }));
  }, [binding.boardDeviceId, onUnbind, push]);

  /* 应急：负一屏物理按键失灵切不回主屏时，远程把设备 .screen-page 写成 main */
  const onDeviceReturnHome = useCallback(async () => {
    try {
      await invoke("usb_set_screen_page", { page: "main" });
      push({ tone: "success", title: "已请求设备切回主屏" });
    } catch (err) {
      push({ tone: "error", title: "切回主屏失败", message: typeof err === "string" ? err : String(err) });
    }
  }, [push]);

  /* 应急：客户端 UI 形象 vs 设备实际脱节时，绕过 desktop-pet-assignment.js 的
   * appearanceChanged 缓存判断，直接重新推送当前 active appearance 到设备 */
  const onForceSyncAppearance = useCallback(async () => {
    const appearanceId =
      (typeof window !== "undefined" && window.localStorage
        ? window.localStorage.getItem(ACTIVE_APPEARANCE_KEY)
        : null) || BUILTIN_TERRIER_APPEARANCE_ID;
    try {
      const result = await invoke("usb_sync_appearance", { appearanceId });
      if (result?.ok) {
        push({
          tone: "success",
          title: `已强制重推形象到设备（${appearanceId}）`,
          message: `${result.fileCount || 0} 个素材，${result.byteCount || 0} bytes`,
        });
      } else {
        push({ tone: "error", title: "强制同步失败", message: result?.error || "未知错误" });
      }
    } catch (err) {
      push({ tone: "error", title: "强制同步失败", message: typeof err === "string" ? err : String(err) });
    }
  }, [push]);

  const selectedVoiceTrigger = VOICE_BUTTON_OPTIONS.find((o) => o.id === voiceConfig.trigger) || VOICE_BUTTON_OPTIONS[0];

  return (
    <PageShell
      title="桌搭控制台"
      help={() => setGuideOpen(true)}
      actions={
        <DashboardActionsMenu
          onSendTest={onSendTest}
          onCopyDesktopId={onCopyDesktopId}
          onUnbind={onUnbindClick}
          onDeviceReturnHome={onDeviceReturnHome}
          onForceSyncAppearance={onForceSyncAppearance}
          onApplyWifi={usb.connected ? () => setWifiModalOpen(true) : undefined}
        />
      }
    >
      <Card>
        <DeviceStatusBar />
      </Card>

      <Card title="Agent与形象">
        <ChannelMatrixCard />
      </Card>

      <Card title="按钮配置" subtitle="按键当前的作用，可直接编辑">
        <BoardButtonPanel
          voiceConfig={voiceConfig}
          buttonActions={voiceConfig.buttonActions}
          voiceConfigDirty={voiceConfigDirty}
          voiceConfigOtaState={voiceConfigOtaState}
          usbConnected={Boolean(usb.connected)}
          selectedTrigger={selectedVoiceTrigger}
          onVoiceConfigChange={updateVoiceConfig}
          onApplyVoiceConfig={applyVoiceConfigOverUsb}
        />
      </Card>

      <Card.Collapsible
        title="语音助手"
        summary={buildVoiceSummary(voiceConfig, selectedVoiceTrigger)}
      >
        <VoiceAssistantPanel
          state={{
            ...voiceState,
            selectedAgentId,
            deviceOnline,
          }}
          dispatch={voiceDispatch}
          toggleAudioBridge={toggleAudioBridge}
          sendMockButtonInject={sendMockButtonInject}
          voiceConfig={voiceConfig}
          selectedTrigger={selectedVoiceTrigger}
          onVoiceConfigChange={updateVoiceConfig}
        />
      </Card.Collapsible>

      <DeviceGuideModal isOpen={guideOpen} onClose={() => setGuideOpen(false)} />
      <WifiApplyModal open={wifiModalOpen} onClose={() => setWifiModalOpen(false)} />
    </PageShell>
  );
}
