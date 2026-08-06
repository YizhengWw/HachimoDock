/**
 * [Input] Verified USB device snapshot, bundled-firmware metadata/update Tauri
 *         commands, firmware progress events, and toast feedback.
 * [Output] Sidebar-only firmware module: older devices show an inline Update
 *          button, equal/newer devices show non-interactive Up to date copy,
 *          and upgrades run in place without a detail page.
 * [Pos] app-shell sidebar node in ref/src/shell.
 * [Sync] If this file changes, update `ref/src/shell/.folder.md` and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { Download, LoaderCircle } from "lucide-react";
import { firmwareUpdateDisposition } from "../lib/firmware-version.js";
import { useToast } from "./ToastStack.jsx";

export default function FirmwareUpdateNavItem({ usb = {} }) {
  const { push } = useToast();
  const [bundledFirmware, setBundledFirmware] = useState(null);
  const [completedVersion, setCompletedVersion] = useState("");
  const [pending, setPending] = useState(false);
  const [percent, setPercent] = useState(0);

  useEffect(() => {
    let cancelled = false;
    invoke("usb_get_bundled_firmware_info")
      .then((info) => {
        if (!cancelled) setBundledFirmware(info);
      })
      .catch((error) => {
        console.warn("[firmware-nav] bundled firmware unavailable", error);
      });
    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    setCompletedVersion("");
    setPercent(0);
  }, [usb.boardDeviceId]);

  useEffect(() => {
    let cancelled = false;
    let unlisten = null;
    listen("usb-firmware-update-progress", (event) => {
      if (!pending) return;
      const nextPercent = Math.max(0, Math.min(100, Number(event?.payload?.percent) || 0));
      setPercent(nextPercent);
    }).then((stop) => {
      if (cancelled) stop();
      else unlisten = stop;
    });
    return () => {
      cancelled = true;
      unlisten?.();
    };
  }, [pending]);

  const currentVersion = completedVersion || String(usb.firmware || "");
  const isDetectedP4 = usb.connected === true
    && usb.runtime === "esp-p4"
    && Boolean(usb.boardDeviceId);
  const disposition = isDetectedP4
    ? firmwareUpdateDisposition(currentVersion, bundledFirmware?.version)
    : "unknown";

  const startUpdate = useCallback(async () => {
    if (pending || disposition !== "update") return;
    setPending(true);
    setPercent(0);
    try {
      const result = await invoke("usb_update_bundled_firmware", {
        expectedBoardDeviceId: usb.boardDeviceId,
      });
      setCompletedVersion(String(result?.version || bundledFirmware?.version || ""));
      setPercent(100);
      push({
        tone: "success",
        title: "设备固件已更新",
        message: `版本 ${result?.version || bundledFirmware?.version || "未知"} 已确认有效。`,
      });
    } catch (error) {
      push({
        tone: "error",
        title: "固件升级失败",
        message: typeof error === "string" ? error : String(error),
      });
    } finally {
      setPending(false);
    }
  }, [bundledFirmware?.version, disposition, pending, push, usb.boardDeviceId]);

  return (
    <div
      className="sidebar-nav__item sidebar-firmware-update"
      aria-label="固件升级"
      title={bundledFirmware?.version
        ? `设备 ${currentVersion || "未知"} · 内置 ${bundledFirmware.version}`
        : "固件升级"}
    >
      {pending ? <LoaderCircle size={16} className="spin" /> : <Download size={16} />}
      <span className="sidebar-nav-label">固件升级</span>
      {disposition === "update" && (
        <button
          type="button"
          className="sidebar-firmware-update__action"
          onClick={startUpdate}
          disabled={pending}
        >
          {pending ? `更新中 ${percent}%` : "更新"}
        </button>
      )}
      {disposition === "latest" && (
        <span className="sidebar-firmware-update__latest">已最新</span>
      )}
    </div>
  );
}
