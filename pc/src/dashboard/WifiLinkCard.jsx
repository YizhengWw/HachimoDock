/**
 * [Input] useDeviceContext usb status (transport, capabilities) and the network_* Tauri commands.
 * [Output] "无线连接" card: USB-only WiFi provisioning (SSID/password), live device WiFi state,
 *          LAN discovery or manual-IP connect to a paired board, and clearing the pairing. The
 *          password is held in component state only until sent; it is never stored or logged.
 * [Pos] component node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md` and `pc/src/lib/wifi-link.js`.
 */

import React, { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import Card from "../shell/Card.jsx";
import Button from "../shell/Button.jsx";
import { useDeviceContext } from "../shell/DeviceContext.jsx";
import { useToast } from "../shell/ToastStack.jsx";
import {
  WIFI_SCOPE_NOTE,
  canProvisionWifi,
  describeWifiStatus,
  isWifiLink,
  linkLabel,
  validateWifiCredentials,
  wifiAddressFromPortName,
  wifiErrorMessage,
  wifiSupported,
} from "../lib/wifi-link.js";

const POLL_MS = 3000;
const errorText = (err) => String(err?.message || err);

export default function WifiLinkCard() {
  const { usb } = useDeviceContext();
  const { push } = useToast();
  const [status, setStatus] = useState(null);
  const [ssid, setSsid] = useState("");
  const [password, setPassword] = useState("");
  const [host, setHost] = useState("");
  const [busy, setBusy] = useState("");
  const [devices, setDevices] = useState([]);
  const mounted = useRef(true);

  const connected = Boolean(usb?.connected);
  const onWifi = isWifiLink(usb);
  const provisionable = canProvisionWifi(usb);
  const board = usb?.boardDeviceId || "";

  const readStatus = useCallback(async () => {
    if (!connected || !wifiSupported(usb)) {
      setStatus(connected ? { supported: false } : null);
      return;
    }
    try {
      const next = await invoke("network_get_status");
      if (mounted.current) setStatus(next);
    } catch (err) {
      if (mounted.current) setStatus({ state: "failed", lastError: String(errorText(err)) });
    }
  }, [connected, usb]);

  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  // Poll while a board is attached so connecting / failed / connected transitions show up live.
  useEffect(() => {
    readStatus();
    if (!connected) return undefined;
    const timer = setInterval(readStatus, POLL_MS);
    return () => clearInterval(timer);
  }, [connected, readStatus]);

  const run = async (label, action, success) => {
    setBusy(label);
    try {
      const result = await action();
      if (success) push({ tone: "success", title: success });
      await readStatus();
      return result;
    } catch (err) {
      push({ tone: "error", title: "WiFi 操作失败", message: wifiErrorMessage(errorText(err)) });
      return null;
    } finally {
      if (mounted.current) setBusy("");
    }
  };

  const handleSave = async () => {
    const problem = validateWifiCredentials(ssid, password);
    if (problem) {
      push({ tone: "warning", title: problem });
      return;
    }
    const reply = await run("save", () => invoke("network_provision_wifi", { ssid, password }), "WiFi 已写入设备，正在连接");
    if (reply) setPassword("");
  };

  const handleClear = () => run("clear", () => invoke("network_clear_wifi"), "已清除设备上的 WiFi 配置");

  const handleDiscover = async () => {
    const found = await run("scan", () => invoke("network_list_devices"));
    if (found) setDevices(found);
  };

  const handleConnect = (boardDeviceId, address) =>
    run("connect", async () => {
      // The context's 3 s usb_get_status poll then reports transport "wifi".
      return invoke("network_connect", { boardDeviceId, host: address || null });
    }, "已通过 WiFi 连接设备");

  const summary = describeWifiStatus(status);
  const subtitle = onWifi
    ? `当前通过 WiFi 连接（${wifiAddressFromPortName(usb.portName)}）`
    : `当前连接：${linkLabel(usb)}`;

  return (
    <Card title="无线连接 (WiFi)" subtitle={subtitle}>
      <p className="message-banner message-banner--muted">{WIFI_SCOPE_NOTE}</p>
      <p className={`message-banner message-banner--${summary.tone === "error" ? "error" : summary.tone === "success" ? "success" : summary.tone === "warning" ? "warning" : "muted"}`} role="status">
        {summary.text}
      </p>

      {provisionable && (
        <div className="wifi-link-form">
          <label className="field">
            <span className="field__label">WiFi 名称</span>
            <input className="field__input" value={ssid} autoComplete="off" onChange={(e) => setSsid(e.target.value)} />
          </label>
          <label className="field">
            <span className="field__label">WiFi 密码</span>
            <input className="field__input" type="password" value={password} autoComplete="new-password" placeholder="开放网络可留空" onChange={(e) => setPassword(e.target.value)} />
          </label>
          <div className="wifi-link-actions">
            <Button variant="primary" loading={busy === "save"} disabled={Boolean(busy)} onClick={handleSave}>保存并连接</Button>
            {status?.configured && (
              <Button variant="danger" loading={busy === "clear"} disabled={Boolean(busy)} onClick={handleClear}>清除 WiFi 配置</Button>
            )}
          </div>
        </div>
      )}
      {connected && !provisionable && (
        <p className="message-banner message-banner--muted">
          {onWifi ? "配置或清除 WiFi 需要用 USB 线连接设备。" : "当前固件不支持 WiFi，请先升级设备固件。"}
        </p>
      )}

      <div className="wifi-link-form">
        <div className="wifi-link-actions">
          <Button variant="secondary" loading={busy === "scan"} disabled={Boolean(busy)} onClick={handleDiscover}>搜索局域网设备</Button>
        </div>
        {devices.map((device) => (
          <div key={device.boardDeviceId} className="wifi-link-device">
            <span>{device.boardDeviceId}（{device.host}{device.online ? "" : "，离线"}）</span>
            {device.desktopPaired ? (
              <Button size="small" disabled={Boolean(busy) || (onWifi && board === device.boardDeviceId)} onClick={() => handleConnect(device.boardDeviceId, device.host)}>
                通过 WiFi 连接
              </Button>
            ) : (
              <span className="field__hint">未与本机配对，请先用 USB 配置</span>
            )}
          </div>
        ))}
        {board && !onWifi && (
          <label className="field">
            <span className="field__label">手动输入设备 IP（可选）</span>
            <input className="field__input" value={host} placeholder="例如 192.168.1.23" onChange={(e) => setHost(e.target.value)} />
            <Button size="small" disabled={Boolean(busy) || !host.trim()} onClick={() => handleConnect(board, host.trim())}>用此地址连接</Button>
          </label>
        )}
      </div>
    </Card>
  );
}
