/**
 * [Input] children tree consuming useDeviceContext; Tauri invoke for USB status/manual serial rescan/WiFi online/bindings/agents; lib helpers for appearance/agent storage.
 * [Output] Single source of polling and derived state (binding, USB status, independent wifiOnline state, derived deviceOnline, appearances, agentAppearanceMap, enabledAgents, agentOptions, currentDisplay, currentComponent); hydrates the active channel from the bridge profile before trusting localStorage cache; Rust owns background serial auto-connect while manual rescan can explicitly scan/connect.
 * [Pos] component node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
} from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import {
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
import { resolveOnlineBoardDeviceId } from "../lib/device-availability.js";
import { listAppearances } from "../lib/appearance-store.js";
import { deriveCurrentDisplay } from "./DeviceContext.pure.js";

export { deriveCurrentDisplay };

const ACTIVE_COMPONENT_STORAGE_KEY = "pet-manager:active-component";

const DeviceContext = createContext(null);

function readActiveComponent() {
  try {
    const raw = localStorage.getItem(ACTIVE_COMPONENT_STORAGE_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    if (parsed && typeof parsed === "object" && parsed.id) return parsed;
  } catch {
    // ignore
  }
  return null;
}

function normalizeUsbStatus(status) {
  return {
    connected: !!status?.connected,
    portName: status?.portName || "",
    boardDeviceId: status?.connected ? status?.boardDeviceId || "" : "",
  };
}

function bridgeEnabledAgents(profile) {
  const selectedAgentId = profile?.selectedAgentId || "";
  if (selectedAgentId) return new Set([selectedAgentId]);
  const enabled = Array.isArray(profile?.enabledAgents)
    ? profile.enabledAgents.filter(Boolean)
    : [];
  return enabled.length > 0 ? new Set([enabled[0]]) : null;
}

export function DeviceContextProvider({ binding: bindingProp, onBindingChange, children }) {
  const [binding, setBindingState] = useState(bindingProp || null);
  useEffect(() => setBindingState(bindingProp || null), [bindingProp]);

  const [usb, setUsb] = useState({ connected: false, portName: "", boardDeviceId: "" });
  const [wifiOnline, setWifiOnline] = useState(false);
  const [wifiBoardDeviceId, setWifiBoardDeviceId] = useState("");
  const [appearances, setAppearances] = useState([]);
  const [agentAppearanceMap, setAgentAppearanceMap] = useState({});
  const [enabledAgents, setEnabledAgents] = useState(new Set());
  const [agentOptions, setAgentOptions] = useState(() =>
    FIXED_AGENT_OPTIONS.map((agent) => ({ ...agent, detected: false })),
  );
  const [currentComponent, setCurrentComponent] = useState(() => readActiveComponent());

  // --- USB status poll (3s); serial auto-connect is owned by the Rust backend. ---
  useEffect(() => {
    let cancelled = false;
    const check = async () => {
      try {
        const status = await invoke("usb_get_status");
        if (cancelled) return;
        setUsb(normalizeUsbStatus(status));
      } catch (err) {
        console.warn("[DeviceContext] usb_get_status failed", err);
      }
    };
    check();
    const id = setInterval(check, 3000);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, []);

  // --- WiFi availability poll (5s); USB status is tracked independently. ---
  useEffect(() => {
    if (!binding) {
      setWifiOnline(false);
      setWifiBoardDeviceId("");
      return undefined;
    }
    let cancelled = false;
    const poll = () => {
      invoke("check_device_availability")
        .then((res) => {
          if (cancelled) return;
          const devices = res?.devices || {};
          const id = resolveOnlineBoardDeviceId(devices, binding);
          setWifiOnline(Boolean(id));
          setWifiBoardDeviceId(id);
        })
        .catch(() => {
          if (!cancelled) {
            setWifiOnline(false);
            setWifiBoardDeviceId("");
          }
        });
    };
    poll();
    const id = setInterval(poll, 5000);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, [binding]);

  // --- Initial: load bridge profile, detect agents, list appearances ---
  const loadAppearancesData = useCallback(async () => {
    try {
      const records = await listAppearances();
      setAppearances(records);
      const map = loadAgentAppearanceMap(records);
      setAgentAppearanceMap(map);
      const enabled = loadEnabledAgents() || new Set();
      setEnabledAgents(enabled);
      return records;
    } catch (err) {
      console.warn("[DeviceContext] listAppearances failed", err);
      return [];
    }
  }, []);

  const loadBridgeSelection = useCallback(async () => {
    try {
      const profile = await invoke("load_bridge_profile");
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

  const detectAgents = useCallback(async () => {
    try {
      const res = await invoke("detect_local_agents");
      const next = normalizeDetectedAgents(res?.agents || []);
      setAgentOptions(next);
    } catch (err) {
      console.warn("[DeviceContext] detect_local_agents failed", err);
    }
  }, []);

  useEffect(() => {
    invoke("load_device_bindings").catch(() => null);
    loadAppearancesData().then(() => loadBridgeSelection());
    detectAgents();
  }, [loadAppearancesData, loadBridgeSelection, detectAgents]);

  // --- Active component updates ---
  // `storage` event fires natively for cross-tab writes. Same-tab writers
  // (e.g. ComponentCenter in Plan 4) must `window.dispatchEvent(new Event("storage"))`
  // after `localStorage.setItem("pet-manager:active-component", ...)` to wake this
  // listener — the native event is cross-tab only.
  useEffect(() => {
    const handler = () => setCurrentComponent(readActiveComponent());
    window.addEventListener("storage", handler);
    return () => window.removeEventListener("storage", handler);
  }, []);

  const refresh = useCallback(async () => {
    await Promise.all([
      loadAppearancesData().then(() => loadBridgeSelection()),
      detectAgents(),
    ]);
  }, [loadAppearancesData, loadBridgeSelection, detectAgents]);

  const rescanUsbDevices = useCallback(async () => {
    const devices = await invoke("usb_scan_devices");
    const list = Array.isArray(devices) ? devices : [];
    const firstPortName = list.find((device) => device?.portName)?.portName;
    if (firstPortName) {
      await invoke("usb_connect", { portName: firstPortName });
    }
    const status = await invoke("usb_get_status");
    const nextUsb = normalizeUsbStatus(status);
    setUsb(nextUsb);
    return { devices: list, status: nextUsb };
  }, []);

  const currentDisplay = useMemo(
    () => deriveCurrentDisplay(agentAppearanceMap, enabledAgents, appearances, agentOptions),
    [agentAppearanceMap, enabledAgents, appearances, agentOptions],
  );

  const deviceOnline = Boolean(usb.connected || wifiOnline);
  const onlineBoardDeviceId = useMemo(
    () => (
      usb.connected
        ? usb.boardDeviceId || wifiBoardDeviceId || binding?.boardDeviceId || ""
        : wifiBoardDeviceId
    ),
    [binding?.boardDeviceId, usb.connected, usb.boardDeviceId, wifiBoardDeviceId],
  );
  const deviceConnected = deviceOnline;

  const applyDesktopPet = useCallback(
    async (agentId, appearance, options = {}) => {
      const { onProgress } = options;
      const currentAppearanceId = currentDisplay.appearance?.id || "";
      const { nextMap, notice } = await applyDesktopPetAssignment({
        invoke,
        listen,
        agentAppearanceMap,
        agentId,
        appearance,
        agentOptions,
        boardDeviceId: onlineBoardDeviceId || usb.boardDeviceId || binding?.boardDeviceId || "",
        currentAppearanceId,
        deviceOnline,
        onProgress,
      });
      setAgentAppearanceMap(nextMap);
      const enabled = new Set(assignedAgentIds(nextMap, agentId));
      setEnabledAgents(enabled);
      saveAgentAppearanceMap(nextMap);
      saveEnabledAgents(enabled);
      return { nextMap, notice };
    },
    [agentAppearanceMap, agentOptions, binding, currentDisplay, deviceOnline, onlineBoardDeviceId, usb.boardDeviceId],
  );

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

  const value = useMemo(
    () => ({
      binding,
      setBinding,
      usb,
      wifiOnline,
      wifiBoardDeviceId,
      deviceOnline,
      onlineBoardDeviceId,
      deviceConnected,
      appearances,
      agentAppearanceMap,
      enabledAgents,
      agentOptions,
      currentDisplay,
      currentComponent,
      applyDesktopPet,
      saveAgentAppearance,
      rescanUsbDevices,
      refresh,
    }),
    [
      binding,
      setBinding,
      usb,
      wifiOnline,
      wifiBoardDeviceId,
      deviceOnline,
      onlineBoardDeviceId,
      deviceConnected,
      appearances,
      agentAppearanceMap,
      enabledAgents,
      agentOptions,
      currentDisplay,
      currentComponent,
      applyDesktopPet,
      saveAgentAppearance,
      rescanUsbDevices,
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
