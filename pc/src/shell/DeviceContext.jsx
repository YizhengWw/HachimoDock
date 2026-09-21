/**
 * [Input] children tree consuming useDeviceContext; Tauri invoke/listen for verified P4 USB status, appearance sync progress, manual serial rescan, bindings, and agents; lib helpers for appearance/agent storage.
 * [Output] Single USB-only source of polling and derived state (binding, USB status, deviceOnline, force-refreshable appearances, agentAppearanceMap, enabledAgents, bridge-authoritative selectedAgentId, agentOptions, agentScan, currentDisplay, target-verified per-device currentComponent, cancellable appearanceSync); USB polling is single-flight and suppresses unchanged Context updates; defaults a truly unconfigured first run to Codex while preserving saved local/Bridge selection, exposes deduplicated manual Agent refresh plus focus-triggered stale refresh, hydrates the active channel from the bridge profile before trusting localStorage cache, and keeps follow changes explicitly USB-only; Rust owns background serial auto-connect while manual rescan can explicitly scan/connect.
 *          Saved-persona events refresh the current target immediately; every conversation start reads fresh disk settings.
 *          Current-key usage help is published by the dashboard and consumed across setup surfaces.
 * [Pos] component node in pc/src/shell
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "react";
import { invoke } from "@tauri-apps/api/core";
import { buildUsageHelp } from "../lib/usage-help.js";
import { listen } from "@tauri-apps/api/event";
import {
  DEFAULT_AGENT_ID,
  FIXED_AGENT_OPTIONS,
  assignAppearanceToAgent,
  assignedAgentIds,
  loadAgentAppearanceMap,
  loadEnabledAgents,
  normalizeDetectedAgents,
  saveAgentAppearanceMap,
  saveEnabledAgents,
} from "../lib/agent-appearance-config.js";
import { applyDesktopPetAssignment } from "../lib/desktop-pet-assignment.js";
import { getAppearance, listAppearances } from "../lib/appearance-store.js";
import { buildRealtimeChatStartInput, PERSONA_VOICE_UPDATED_EVENT } from "../lib/persona-voice.js";
import {
  readActiveComponentForTarget,
  readConfiguredComponentSshHost,
} from "../lib/active-component-store.js";
import {
  deriveCurrentDisplay,
  deriveDeviceReachability,
  normalizeUsbStatusPayload,
  usbStatusSnapshotsEqual,
} from "./DeviceContext.pure.js";

export { deriveCurrentDisplay };

const EMPTY_APPEARANCE_SYNC = {
  pending: false,
  cancelling: false,
  progress: null,
  agentId: "",
  appearanceId: "",
  appearanceName: "",
};

const AGENT_SCAN_FOCUS_STALE_MS = 5 * 60_000;
const USB_STATUS_POLL_MS = 3_000;
const USB_STATUS_SLOW_WARNING_MS = 10_000;

const DeviceContext = createContext(null);
const EMPTY_USAGE_HELP = buildUsageHelp();

function bridgeEnabledAgents(profile) {
  const selectedAgentId = profile?.selectedAgentId || "";
  if (selectedAgentId) return new Set([selectedAgentId]);
  const enabled = Array.isArray(profile?.enabledAgents)
    ? profile.enabledAgents.filter(Boolean)
    : [];
  return enabled.length > 0 ? new Set([enabled[0]]) : null;
}

export function DeviceContextProvider({ binding: bindingProp, onBindingChange, children }) {
  const [usageHelp, setUsageHelp] = useState(EMPTY_USAGE_HELP);
  const [binding, setBindingState] = useState(bindingProp || null);
  useEffect(() => setBindingState(bindingProp || null), [bindingProp]);
  useEffect(() => { if (!binding) setUsageHelp(EMPTY_USAGE_HELP); }, [binding]);

  const [usb, setUsb] = useState({ connected: false, portName: "", boardDeviceId: "" });
  const [appearances, setAppearances] = useState([]);
  const [agentAppearanceMap, setAgentAppearanceMap] = useState({});
  const [enabledAgents, setEnabledAgents] = useState(
    () => new Set([DEFAULT_AGENT_ID]),
  );
  const [bridgeSelectedAgentId, setBridgeSelectedAgentId] = useState("");
  const [agentOptions, setAgentOptions] = useState(() =>
    FIXED_AGENT_OPTIONS.map((agent) => ({ ...agent, detected: false })),
  );
  const [agentScan, setAgentScan] = useState({
    pending: false,
    scannedAt: 0,
    detectedCount: 0,
    error: "",
  });
  const agentScanRequestRef = useRef(null);
  const lastAgentScanAttemptAtRef = useRef(0);
  const [activeComponentRevision, setActiveComponentRevision] = useState(0);
  const currentComponentTarget = useMemo(() => {
    const usbBoardDeviceId = String(usb.boardDeviceId || "").trim();
    if (usb.connected && usbBoardDeviceId) {
      return { transport: "usb", boardDeviceId: usbBoardDeviceId };
    }
    const sshHost = readConfiguredComponentSshHost();
    if (sshHost) return { transport: "ssh", sshHost };
    const rememberedBoardDeviceId = String(binding?.boardDeviceId || "").trim();
    return rememberedBoardDeviceId
      ? { transport: "usb", boardDeviceId: rememberedBoardDeviceId }
      : null;
  }, [
    binding?.boardDeviceId,
    usb.boardDeviceId,
    usb.connected,
  ]);
  const currentComponent = useMemo(
    () => readActiveComponentForTarget(currentComponentTarget),
    [activeComponentRevision, currentComponentTarget],
  );
  const [appearanceSync, setAppearanceSync] = useState(EMPTY_APPEARANCE_SYNC);
  const appearanceSyncTokenRef = useRef(0);
  // 实时对话（2026-09-18）：Rust 持有会话，这里只镜像状态并把板端按键请求解析成当前形象。
  const [realtimeChat, setRealtimeChat] = useState({ active: false, state: "idle", error: "", reason: "" });

  // --- USB status poll (3s); serial auto-connect is owned by the Rust backend. ---
  useEffect(() => {
    let cancelled = false;
    let inFlight = false;
    let slowWarningTimer = null;
    const check = async () => {
      if (cancelled || inFlight) return;
      inFlight = true;
      slowWarningTimer = setTimeout(() => {
        console.warn("[DeviceContext] usb_get_status is taking longer than expected");
      }, USB_STATUS_SLOW_WARNING_MS);
      try {
        const status = await invoke("usb_get_status");
        if (cancelled) return;
        const nextUsb = normalizeUsbStatusPayload(status);
        setUsb((currentUsb) => (
          usbStatusSnapshotsEqual(currentUsb, nextUsb) ? currentUsb : nextUsb
        ));
      } catch (err) {
        if (cancelled) return;
        console.warn("[DeviceContext] usb_get_status failed", err);
      } finally {
        if (slowWarningTimer) clearTimeout(slowWarningTimer);
        slowWarningTimer = null;
        inFlight = false;
      }
    };
    check();
    const id = setInterval(check, USB_STATUS_POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(id);
      if (slowWarningTimer) clearTimeout(slowWarningTimer);
    };
  }, []);

  // --- Initial: load bridge profile, detect agents, list appearances ---
  const loadAppearancesData = useCallback(async ({ force = false } = {}) => {
    try {
      const records = await listAppearances({ force });
      setAppearances(records);
      const map = loadAgentAppearanceMap(records);
      setAgentAppearanceMap(map);
      const enabled = loadEnabledAgents() || new Set([DEFAULT_AGENT_ID]);
      setEnabledAgents(enabled);
      return records;
    } catch (err) {
      console.warn("[DeviceContext] listAppearances failed", err);
      return [];
    }
  }, []);

  const refreshAppearances = useCallback(
    () => loadAppearancesData({ force: true }),
    [loadAppearancesData],
  );

  const loadBridgeSelection = useCallback(async () => {
    try {
      const profile = await invoke("load_bridge_profile");
      setBridgeSelectedAgentId(profile?.selectedAgentId || profile?.enabledAgents?.[0] || "");
      const bridgeEnabled = bridgeEnabledAgents(profile);
      if (bridgeEnabled) {
        setEnabledAgents(bridgeEnabled);
        saveEnabledAgents(bridgeEnabled);
      }
      return bridgeEnabled;
    } catch (err) {
      console.warn("[DeviceContext] load_bridge_profile failed", err);
      return null;
    }
  }, []);

  const refreshAgents = useCallback(async () => {
    if (agentScanRequestRef.current) return agentScanRequestRef.current;

    setAgentScan((current) => ({ ...current, pending: true, error: "" }));
    const request = (async () => {
      try {
        const res = await invoke("detect_local_agents");
        const next = normalizeDetectedAgents(res?.agents || []);
        const scannedAt = Number(res?.scannedAt || res?.scanned_at) || Date.now();
        setAgentOptions(next);
        setAgentScan({
          pending: false,
          scannedAt,
          detectedCount: next.filter((agent) => agent.detected).length,
          error: "",
        });
        return next;
      } catch (err) {
        const message = err?.message || String(err);
        setAgentScan((current) => ({
          ...current,
          pending: false,
          error: message,
        }));
        console.warn("[DeviceContext] detect_local_agents failed", err);
        throw err;
      } finally {
        lastAgentScanAttemptAtRef.current = Date.now();
      }
    })();

    agentScanRequestRef.current = request;
    try {
      return await request;
    } finally {
      if (agentScanRequestRef.current === request) {
        agentScanRequestRef.current = null;
      }
    }
  }, []);

  useEffect(() => {
    invoke("load_device_bindings").catch(() => null);
    loadAppearancesData().then(() => loadBridgeSelection());
    refreshAgents().catch(() => null);
  }, [loadAppearancesData, loadBridgeSelection, refreshAgents]);

  useEffect(() => {
    const refreshIfStale = () => {
      if (document.visibilityState === "hidden") return;
      const elapsed = Date.now() - lastAgentScanAttemptAtRef.current;
      if (elapsed < AGENT_SCAN_FOCUS_STALE_MS) return;
      refreshAgents().catch(() => null);
    };

    window.addEventListener("focus", refreshIfStale);
    document.addEventListener("visibilitychange", refreshIfStale);
    return () => {
      window.removeEventListener("focus", refreshIfStale);
      document.removeEventListener("visibilitychange", refreshIfStale);
    };
  }, [refreshAgents]);

  // --- Active component updates ---
  // `storage` event fires natively for cross-tab writes. Same-tab writers
  // (e.g. ComponentCenter in Plan 4) must `window.dispatchEvent(new Event("storage"))`
  // after `localStorage.setItem("pet-manager:active-component", ...)` to wake this
  // listener — the native event is cross-tab only.
  useEffect(() => {
    const handler = () => setActiveComponentRevision((revision) => revision + 1);
    window.addEventListener("storage", handler);
    return () => window.removeEventListener("storage", handler);
  }, []);

  const refresh = useCallback(async () => {
    await Promise.all([
      refreshAppearances().then(() => loadBridgeSelection()),
      refreshAgents().catch(() => null),
    ]);
  }, [loadBridgeSelection, refreshAgents, refreshAppearances]);

  const rescanUsbDevices = useCallback(async () => {
    const devices = await invoke("usb_scan_devices");
    const list = Array.isArray(devices) ? devices : [];
    let connectedStatus = null;
    let lastError = null;
    for (const device of list) {
      const portName = device?.portName;
      if (!portName) continue;
      try {
        const candidateStatus = normalizeUsbStatusPayload(
          await invoke("usb_connect", { portName }),
        );
        if (candidateStatus.connected) {
          connectedStatus = candidateStatus;
          break;
        }
      } catch (error) {
        lastError = error;
      }
    }
    const nextUsb = connectedStatus || normalizeUsbStatusPayload(await invoke("usb_get_status"));
    setUsb(nextUsb);
    if (!nextUsb.connected && lastError) throw lastError;
    return { devices: list, status: nextUsb };
  }, []);

  const currentDisplay = useMemo(
    () => deriveCurrentDisplay(agentAppearanceMap, enabledAgents, appearances, agentOptions),
    [agentAppearanceMap, enabledAgents, appearances, agentOptions],
  );

  const { deviceOnline, onlineBoardDeviceId } = useMemo(
    () => deriveDeviceReachability({ usb }),
    [usb],
  );
  const deviceConnected = deviceOnline;

  const applyDesktopPet = useCallback(
    async (agentId, appearance, options = {}) => {
      const { initialProgress, onProgress } = options;
      const syncToken = appearanceSyncTokenRef.current + 1;
      appearanceSyncTokenRef.current = syncToken;
      const appearanceSyncMeta = {
        agentId,
        appearanceId: appearance?.id || "",
        appearanceName: appearance?.name || "",
      };
      const emitAppearanceSyncProgress = (progress) => {
        setAppearanceSync((current) => ({
          pending: true,
          cancelling: current.cancelling,
          progress,
          ...appearanceSyncMeta,
        }));
        onProgress?.(progress);
      };
      emitAppearanceSyncProgress(initialProgress || {
        text: appearance?.name
          ? `正在切换到「${appearance.name}」，优先使用设备已有素材…`
          : "正在切换形象，优先使用设备已有素材…",
        percent: 0,
      });
      const currentAppearanceId = currentDisplay.appearance?.id || "";
      try {
        const { nextMap, notice } = await applyDesktopPetAssignment({
          invoke,
          listen,
          agentAppearanceMap,
          agentId,
          appearance,
          agentOptions,
          boardDeviceId: usb.boardDeviceId || onlineBoardDeviceId || binding?.boardDeviceId || "",
          currentAppearanceId,
          onProgress: emitAppearanceSyncProgress,
        });
        setAgentAppearanceMap(nextMap);
        const enabled = new Set(assignedAgentIds(nextMap, agentId));
        setEnabledAgents(enabled);
        setBridgeSelectedAgentId(agentId);
        saveAgentAppearanceMap(nextMap);
        saveEnabledAgents(enabled);
        return { nextMap, notice };
      } finally {
        if (appearanceSyncTokenRef.current === syncToken) {
          setAppearanceSync({
            pending: false,
            cancelling: false,
            progress: null,
            ...appearanceSyncMeta,
          });
        }
      }
    },
    [agentAppearanceMap, agentOptions, binding, currentDisplay, onlineBoardDeviceId, usb.boardDeviceId],
  );

  const cancelAppearanceSync = useCallback(async () => {
    if (!appearanceSync.pending || appearanceSync.cancelling) {
      return { requested: false };
    }
    setAppearanceSync((current) => ({
      ...current,
      cancelling: true,
      progress: current.progress
        ? { ...current.progress, text: "正在中断 USB 形象传输…" }
        : { text: "正在中断 USB 形象传输…", percent: 0 },
    }));
    try {
      const result = await invoke("usb_cancel_appearance_sync");
      if (!result?.requested) {
        setAppearanceSync((current) => ({ ...current, cancelling: false }));
      }
      return result;
    } catch (error) {
      setAppearanceSync((current) => ({ ...current, cancelling: false }));
      throw error;
    }
  }, [appearanceSync.cancelling, appearanceSync.pending]);

  const saveAgentAppearance = useCallback((agentId, appearanceId) => {
    const nextMap = assignAppearanceToAgent(agentAppearanceMap, agentId, appearanceId);
    setAgentAppearanceMap(nextMap);
    saveAgentAppearanceMap(nextMap);
    return nextMap;
  }, [agentAppearanceMap]);

  const setBinding = useCallback(
    (next) => {
      setBindingState(next);
      onBindingChange?.(next);
    },
    [onBindingChange],
  );

  const realtimeTargetRef = useRef({ appearance: null, boardDeviceId: "" });
  useEffect(() => {
    const onPersonaSaved = (event) => {
      const { appearanceId, personaVoice } = event.detail || {};
      if (!appearanceId || !personaVoice) return;
      setAppearances((records) => records.map((record) => record.id === appearanceId ? { ...record, personaVoice } : record));
      const target = realtimeTargetRef.current;
      if (target.appearance?.id === appearanceId) {
        realtimeTargetRef.current = { ...target, appearance: { ...target.appearance, personaVoice } };
      }
    };
    window.addEventListener(PERSONA_VOICE_UPDATED_EVENT, onPersonaSaved);
    return () => window.removeEventListener(PERSONA_VOICE_UPDATED_EVENT, onPersonaSaved);
  }, []);
  useEffect(() => {
    realtimeTargetRef.current = {
      appearance: currentDisplay.appearance || null,
      boardDeviceId: String(usb.boardDeviceId || "").trim(),
    };
  }, [currentDisplay.appearance, usb.boardDeviceId]);

  const startRealtimeChat = useCallback(async (options = {}) => {
    const target = realtimeTargetRef.current;
    const selectedAppearance = options.appearance || target.appearance;
    const appearance = selectedAppearance ? await getAppearance(selectedAppearance.id) : null;
    const boardDeviceId = String(options.boardDeviceId || target.boardDeviceId || "").trim();
    if (!appearance) throw new Error("设备上还没有形象，先在画廊里应用一个形象");
    if (!appearance.personaVoice?.configured) {
      throw new Error(`「${appearance.name}」还没有设置人设与声音，先在画廊卡片右下角设置`);
    }
    if (!boardDeviceId) throw new Error("设备未通过 USB 连接");
    const status = await invoke("realtime_chat_start", { input: buildRealtimeChatStartInput(appearance, boardDeviceId) });
    setRealtimeChat((current) => ({ ...current, ...status, error: "" }));
    return status;
  }, []);

  const stopRealtimeChat = useCallback(async (reason = "ui") => {
    const status = await invoke("realtime_chat_stop", { reason });
    setRealtimeChat((current) => ({ ...current, ...status }));
    return status;
  }, []);

  useEffect(() => {
    let disposed = false;
    const unlisteners = [];
    invoke("realtime_chat_status")
      .then((status) => { if (!disposed && status) setRealtimeChat((current) => ({ ...current, ...status })); })
      .catch(() => {});
    listen("realtime-chat-state", (event) => {
      const status = event?.payload;
      if (status && typeof status === "object") setRealtimeChat((current) => ({ ...current, ...status }));
    }).then((stop) => { if (disposed) stop(); else unlisteners.push(stop); }).catch(() => {});
    // Recover the final error even if a fast failure raced listener registration.
    const refresh = setInterval(() => {
      invoke("realtime_chat_status").then((status) => {
        if (!disposed && status?.sessionId) setRealtimeChat((current) => ({ ...current, ...status }));
      }).catch(() => {});
    }, 2000);
    // 板端「实时对话」键：Rust 不知道设备上是哪个形象，由这里用当前形象拉起会话。
    listen("realtime-chat-request", (event) => {
      const boardDeviceId = String(event?.payload?.boardDeviceId || "").trim();
      startRealtimeChat({ boardDeviceId }).catch((error) => {
        const message = error?.message || String(error);
        setRealtimeChat((current) => ({ ...current, active: false, state: "idle", error: message }));
        invoke("usb_send_speech", { text: message.slice(0, 60) }).catch(() => {});
      });
    }).then((stop) => { if (disposed) stop(); else unlisteners.push(stop); }).catch(() => {});
    return () => {
      disposed = true;
      clearInterval(refresh);
      unlisteners.forEach((stop) => { try { stop(); } catch { /* ignore */ } });
    };
  }, [startRealtimeChat]);

  const value = useMemo(
    () => ({
      binding,
      setBinding,
      usb,
      deviceOnline,
      onlineBoardDeviceId,
      deviceConnected,
      appearances,
      agentAppearanceMap,
      enabledAgents,
      selectedAgentId: bridgeSelectedAgentId,
      agentOptions,
      agentScan,
      currentDisplay,
      currentComponent,
      currentComponentTarget,
      appearanceSync,
      realtimeChat,
      usageHelp,
      setUsageHelp,
      startRealtimeChat,
      stopRealtimeChat,
      applyDesktopPet,
      cancelAppearanceSync,
      saveAgentAppearance,
      rescanUsbDevices,
      refreshAgents,
      refreshAppearances,
      refresh,
    }),
    [
      binding,
      setBinding,
      usb,
      deviceOnline,
      onlineBoardDeviceId,
      deviceConnected,
      appearances,
      agentAppearanceMap,
      enabledAgents,
      bridgeSelectedAgentId,
      agentOptions,
      agentScan,
      currentDisplay,
      currentComponent,
      currentComponentTarget,
      appearanceSync,
      realtimeChat,
      usageHelp,
      startRealtimeChat,
      stopRealtimeChat,
      applyDesktopPet,
      cancelAppearanceSync,
      saveAgentAppearance,
      rescanUsbDevices,
      refreshAgents,
      refreshAppearances,
      refresh,
    ],
  );

  return <DeviceContext.Provider value={value}>{children}</DeviceContext.Provider>;
}

export function useDeviceContext() {
  const ctx = useContext(DeviceContext);
  if (!ctx) throw new Error("useDeviceContext must be used inside <DeviceContextProvider>");
  return ctx;
}

export function useUsageHelp() {
  return useContext(DeviceContext)?.usageHelp || EMPTY_USAGE_HELP;
}
