/**
 * [Input] useDeviceContext for binding/usb/rescanUsbDevices; useToast for manual scan result notices.
 * [Output] Region 1 of the P4 dashboard: single-row card showing board id, verified USB state, and a manual USB serial rescan button.
 * [Pos] component node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import React, { useState } from "react";
import { Monitor, RefreshCw, Usb, Wifi } from "lucide-react";
import { useDeviceContext } from "../shell/DeviceContext.jsx";
import { useToast } from "../shell/ToastStack.jsx";
import { isWifiLink, linkLabel, wifiAddressFromPortName } from "../lib/wifi-link.js";

export default function DeviceStatusBar() {
  const { binding, usb, rescanUsbDevices } = useDeviceContext();
  const { push } = useToast();
  const [scanningUsb, setScanningUsb] = useState(false);

  const handleRescanUsb = async () => {
    setScanningUsb(true);
    try {
      const result = await rescanUsbDevices();
      if (result?.status?.connected) {
        push({
          tone: "success",
          title: `已连接 USB 串口 ${result.status.portName || ""}`.trim(),
        });
      } else if (result?.devices?.length) {
        push({
          tone: "warning",
          title: "已扫描到 USB 串口，但连接未完成",
          message: result.devices.map((device) => device.portName).join(" / "),
        });
      } else {
        push({
          tone: "warning",
          title: "未检测到 USB 串口设备",
          message: "请确认设备端 USB gadget 已启动，或重新插拔线缆后再扫描。",
        });
      }
    } catch (err) {
      push({
        tone: "error",
        title: "USB 串口扫描失败",
        message: String(err?.message || err),
      });
    } finally {
      setScanningUsb(false);
    }
  };

  if (!binding) return null;

  const legacyTransportLabel = String(binding.wifiSsid || "").trim();
  const usbOnlyBindingMatch = legacyTransportLabel.match(/^USB\((.+)\)$/);
  const savedUsbPortName = usbOnlyBindingMatch?.[1] || "";
  const wifiLink = isWifiLink(usb);
  const usbPortLabel = usb.connected
    ? (wifiLink ? wifiAddressFromPortName(usb.portName) : usb.portName) || savedUsbPortName || "已连接"
    : "未连接";
  const usbChip = {
    className: `dashboard-status-bar__chip dashboard-status-bar__chip--${usb.connected ? "ok" : "warn"}`,
    icon: wifiLink ? <Wifi size={14} /> : <Usb size={14} />,
    label: usb.connected ? linkLabel(usb) : "USB 未连接",
  };
  return (
    <div className="dashboard-status-bar">
      <span className="dashboard-status-bar__icon">
        <Monitor size={18} />
      </span>
      <div className="dashboard-status-bar__copy">
        <strong className="dashboard-status-bar__board-id">{binding.boardDeviceId}</strong>
        <span className="dashboard-status-bar__sub">
          {wifiLink ? "WiFi" : "USB"}: {usbPortLabel}
        </span>
      </div>
      <div className="dashboard-status-bar__chips">
        <span className={usbChip.className}>
          {usbChip.icon}
          {usbChip.label}
        </span>
      </div>
      <button
        type="button"
        className="dashboard-status-bar__scan-btn"
        onClick={handleRescanUsb}
        disabled={scanningUsb}
        title="重新扫描 USB 串口设备"
      >
        <RefreshCw size={14} className={scanningUsb ? "spin" : undefined} />
        {scanningUsb ? "扫描中..." : "重新扫描串口"}
      </button>
    </div>
  );
}
