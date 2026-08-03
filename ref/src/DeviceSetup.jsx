/**
 * [Input] Tauri USB scan/connect/status commands and persisted device bindings.
 * [Output] USB-only first-run surface that automatically discovers a verified Pet Manager device, persists its binding, starts the shared deduplicated Bridge runtime, and enters the dashboard; exposes a manual rescan when no device is present.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { ensureBridgeRuntime } from "./lib/bridge-runtime.js";
import {
  Cable,
  CheckCircle2,
  Loader2,
  RefreshCw,
  Usb,
} from "lucide-react";

const AUTO_SCAN_INTERVAL_MS = 3000;

function hasTauriRuntime() {
  return typeof window !== "undefined" && Boolean(window.__TAURI_INTERNALS__);
}

function stableBindings(bindings) {
  return Array.isArray(bindings)
    ? bindings.filter((binding) => String(binding?.boardDeviceId || "").trim())
    : [];
}

function normalizedUsbStatus(status) {
  return {
    connected: Boolean(status?.connected),
    portName: String(status?.portName || "").trim(),
    boardDeviceId: String(status?.boardDeviceId || "").trim(),
    runtime: String(status?.runtime || "").trim(),
  };
}

export default function DeviceSetup({ onComplete } = {}) {
  const [phase, setPhase] = useState("scanning");
  const [message, setMessage] = useState("正在自动查找已连接的桌宠…");
  const [device, setDevice] = useState(null);
  const scanInFlightRef = useRef(null);
  const completedRef = useRef(false);
  const mountedRef = useRef(true);

  const finish = useCallback(async () => {
    if (completedRef.current) return;
    completedRef.current = true;
    await onComplete?.();
  }, [onComplete]);

  const persistConnectedDevice = useCallback(async (rawStatus) => {
    const status = normalizedUsbStatus(rawStatus);
    if (!status.connected || !status.boardDeviceId) {
      throw new Error("USB 已连接，但设备没有返回有效身份");
    }

    const desktopDeviceId = await invoke("get_or_create_desktop_device_id");
    await invoke("save_device_binding", {
      binding: {
        boardDeviceId: status.boardDeviceId,
        desktopDeviceId,
        wifiSsid: `USB(${status.portName || "direct"})`,
        boundAt: Date.now(),
      },
    });
    ensureBridgeRuntime().catch(() => {});

    if (!mountedRef.current) return;
    setDevice(status);
    setPhase("connected");
    setMessage("设备已连接，正在进入管理页面…");
    await finish();
  }, [finish]);

  const scan = useCallback(async ({ quiet = false } = {}) => {
    if (completedRef.current) return;
    if (scanInFlightRef.current) return scanInFlightRef.current;

    const request = (async () => {
      if (!quiet && mountedRef.current) {
        setPhase("scanning");
        setMessage("正在扫描 USB 设备…");
      }

      try {
        const bindings = stableBindings(await invoke("load_device_bindings"));
        if (bindings.length > 0) {
          await finish();
          return;
        }

        let status = normalizedUsbStatus(await invoke("usb_get_status"));
        if (!status.connected || !status.boardDeviceId) {
          const candidates = await invoke("usb_scan_devices");
          for (const candidate of Array.isArray(candidates) ? candidates : []) {
            const portName = String(candidate?.portName || "").trim();
            if (!portName) continue;
            try {
              status = normalizedUsbStatus(await invoke("usb_connect", { portName }));
              if (status.connected && status.boardDeviceId) break;
            } catch {
              // A serial port is not necessarily a Pet Manager device. Keep probing.
            }
          }
        }

        if (status.connected && status.boardDeviceId) {
          await persistConnectedDevice(status);
          return;
        }

        if (mountedRef.current) {
          setDevice(null);
          setPhase("waiting");
          setMessage("暂未检测到桌宠，请确认设备已开机并连接数据线。");
        }
      } catch (error) {
        if (mountedRef.current) {
          setDevice(null);
          setPhase("error");
          setMessage(error?.message || String(error));
        }
      }
    })();

    scanInFlightRef.current = request;
    try {
      await request;
    } finally {
      if (scanInFlightRef.current === request) {
        scanInFlightRef.current = null;
      }
    }
  }, [finish, persistConnectedDevice]);

  useEffect(() => {
    mountedRef.current = true;
    scan();
    const intervalId = window.setInterval(
      () => scan({ quiet: true }),
      AUTO_SCAN_INTERVAL_MS,
    );
    return () => {
      mountedRef.current = false;
      window.clearInterval(intervalId);
    };
  }, [scan]);

  if (!hasTauriRuntime()) {
    return (
      <div className="setup--wizard usb-setup">
        <section className="usb-setup-card">
          <span className="usb-setup-card__icon">
            <Usb size={28} />
          </span>
          <h2>请在 Pet Manager 客户端中连接设备</h2>
          <p>浏览器预览不支持 USB 设备发现。</p>
        </section>
      </div>
    );
  }

  const busy = phase === "scanning" || phase === "connected";

  return (
    <div className="setup--wizard usb-setup">
      <section className="usb-setup-card" aria-live="polite">
        <div className={`usb-setup-card__icon usb-setup-card__icon--${phase}`}>
          {phase === "connected"
            ? <CheckCircle2 size={30} />
            : busy
              ? <Loader2 size={30} className="spin" />
              : <Usb size={30} />}
        </div>

        <div className="usb-setup-card__copy">
          <span className="usb-setup-card__eyebrow">USB 自动连接</span>
          <h2>{phase === "connected" ? "已发现桌宠" : "连接你的桌宠"}</h2>
          <p>{message}</p>
        </div>

        {device ? (
          <dl className="usb-setup-device">
            <div>
              <dt>设备</dt>
              <dd>{device.boardDeviceId}</dd>
            </div>
            <div>
              <dt>接口</dt>
              <dd>{device.portName || "USB"}</dd>
            </div>
          </dl>
        ) : (
          <div className="usb-setup-checklist">
            <span><Cable size={18} /> 使用可传输数据的 USB 线连接设备</span>
            <span><CheckCircle2 size={18} /> 保持设备开机，Pet Manager 会自动识别</span>
          </div>
        )}

        <button
          className="btn-primary usb-setup-card__action"
          type="button"
          disabled={busy}
          onClick={() => scan()}
        >
          {busy ? <Loader2 size={16} className="spin" /> : <RefreshCw size={16} />}
          {busy ? "正在检测" : "重新扫描"}
        </button>

        {phase === "error" ? (
          <p className="usb-setup-card__error">检测失败：{message}</p>
        ) : null}
      </section>
    </div>
  );
}
