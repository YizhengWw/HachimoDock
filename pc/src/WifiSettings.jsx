/**
 * [Input] Tauri usb_send_topic command, usb-message events, and the firmware
 *         wifi/status, wifi/scan, wifi/config and wifi/reset topics.
 * [Output] USB-based Wi-Fi provisioning and status panel; the password is never
 *          persisted by the UI and the Wi-Fi TCP data transport is a later stage.
 * [Pos] Wi-Fi provisioning modal launched from the device dashboard status bar.
 * [Sync] If this file changes, update pc/.folder.md.
 */

import React, { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import {
  Activity,
  AlertCircle,
  CheckCircle2,
  Loader2,
  RefreshCw,
  Wifi,
} from "lucide-react";

const RESPONSE_TIMEOUT_MS = 8000;
const STATUS_POLL_MS = 5000;

function normalizeStatus(payload = {}) {
  return {
    connected: Boolean(payload.connected),
    clientConnected: Boolean(payload.clientConnected),
    ip: String(payload.ip || ""),
    rssi: Number(payload.rssi || 0),
    c6Version: String(payload.c6Version || "unknown"),
    wifiTcpCompatible: Boolean(payload.wifiTcpCompatible),
  };
}

export default function WifiSettings() {
  const [status, setStatus] = useState(normalizeStatus());
  const [aps, setAps] = useState([]);
  const [ssid, setSsid] = useState("");
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState("");
  const [message, setMessage] = useState("");
  const [tone, setTone] = useState("muted");
  const timeoutRef = useRef(null);
  const statusPendingRef = useRef(false);

  const armTimeout = useCallback((pendingAction) => {
    if (timeoutRef.current) clearTimeout(timeoutRef.current);
    timeoutRef.current = setTimeout(() => {
      setBusy((current) => (current === pendingAction ? "" : current));
      setMessage("设备没有在预期时间内返回结果；请确认 USB 连接正常后重试。");
      setTone("warning");
    }, RESPONSE_TIMEOUT_MS);
  }, []);

  const clearBusy = useCallback(() => {
    if (timeoutRef.current) clearTimeout(timeoutRef.current);
    timeoutRef.current = null;
    setBusy("");
  }, []);

  const sendTopic = useCallback(async (topic, payload = {}) => {
    try {
      await invoke("usb_send_topic", { topic, payload });
      return true;
    } catch (error) {
      setMessage(`发送失败：${error}`);
      setTone("error");
      return false;
    }
  }, []);

  const requestStatus = useCallback(async (silent = false) => {
    try {
      await invoke("usb_send_topic", { topic: "wifi/status", payload: {} });
      return true;
    } catch (error) {
      if (!silent) {
        setMessage(`读取 Wi-Fi 状态失败：${error}`);
        setTone("error");
      }
      return false;
    }
  }, []);

  useEffect(() => {
    let disposed = false;
    let unlisten = null;

    listen("usb-message", (event) => {
      if (disposed) return;
      const message = event?.payload || {};
      const topic = String(message.topic || "");
      const payload = message.payload || {};
      if (topic === "wifi/status_result") {
        setStatus(normalizeStatus(payload));
        if (payload && payload.ok === false) {
          setMessage(String(payload.error || "读取 Wi-Fi 状态失败"));
          setTone("error");
        }
        if (statusPendingRef.current) {
          statusPendingRef.current = false;
          clearBusy();
        }
        return;
      }
      if (topic === "wifi/scan_result") {
        clearBusy();
        if (payload.ok === false) {
          setMessage(`扫描失败：${payload.error || "未知错误"}`);
          setTone("error");
          return;
        }
        const nextAps = Array.isArray(payload.aps)
          ? [...payload.aps].sort((a, b) => Number(b.rssi || 0) - Number(a.rssi || 0))
          : [];
        setAps(nextAps);
        setSsid((current) => current || nextAps[0]?.ssid || "");
        setMessage(nextAps.length ? `扫描到 ${nextAps.length} 个 2.4GHz 网络` : "没有扫描到 2.4GHz 网络");
        setTone(nextAps.length ? "success" : "warning");
        return;
      }
      if (topic === "wifi/config_result") {
        clearBusy();
        if (payload.ok === false) {
          setMessage(`保存失败：${payload.error || "未知错误"}`);
          setTone("error");
          return;
        }
        setPassword("");
        setMessage("Wi-Fi 凭据已保存，正在连接；稍后查看状态。");
        setTone("success");
        setTimeout(() => { if (!disposed) requestStatus(); }, 2500);
        return;
      }
      if (topic === "wifi/reset_result") {
        clearBusy();
        if (payload.ok === false) {
          setMessage(`清除失败：${payload.error || "未知错误"}`);
          setTone("error");
          return;
        }
        setStatus(normalizeStatus());
        setMessage("已清除设备保存的 Wi-Fi 凭据。");
        setTone("success");
      }
    }).then((off) => {
      if (disposed) off();
      else unlisten = off;
    }).catch((error) => {
      setMessage(`监听设备消息失败：${error}`);
      setTone("error");
    });

    requestStatus(true);
    const poll = setInterval(() => {
      if (!disposed) requestStatus(true);
    }, STATUS_POLL_MS);

    return () => {
      disposed = true;
      clearInterval(poll);
      if (timeoutRef.current) clearTimeout(timeoutRef.current);
      if (unlisten) unlisten();
    };
    // requestStatus is stable; busy intentionally not part of this subscription.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [requestStatus, clearBusy]);

  const scan = async () => {
    setBusy("scan");
    setMessage("正在扫描附近 2.4GHz Wi-Fi…");
    setTone("muted");
    setAps([]);
    armTimeout("scan");
    if (!(await sendTopic("wifi/scan"))) {
      clearBusy();
    }
  };

  const configure = async () => {
    const trimmedSsid = ssid.trim();
    if (!trimmedSsid) {
      setMessage("请先选择或输入 Wi-Fi SSID。");
      setTone("warning");
      return;
    }
    if (!password) {
      setMessage("请输入 Wi-Fi 密码。");
      setTone("warning");
      return;
    }
    setBusy("config");
    setMessage("正在保存 Wi-Fi 凭据…");
    setTone("muted");
    armTimeout("config");
    const ok = await sendTopic("wifi/config", { ssid: trimmedSsid, password });
    if (!ok) clearBusy();
  };

  const reset = async () => {
    setBusy("reset");
    setMessage("正在清除设备 Wi-Fi 凭据…");
    setTone("muted");
    armTimeout("reset");
    if (!(await sendTopic("wifi/reset"))) clearBusy();
  };

  const refresh = async () => {
    setBusy("status");
    armTimeout("status");
    statusPendingRef.current = true;
    if (!(await requestStatus(true))) {
      statusPendingRef.current = false;
      clearBusy();
    }
  };

  const wifiTone = tone === "error" || tone === "warning" ? tone : tone === "success" ? "success" : "muted";

  return (
    <div className="wifi-settings">
      <div className={`wifi-settings__banner wifi-settings__banner--${wifiTone}`} role="status">
        {wifiTone === "error" || wifiTone === "warning" ? <AlertCircle size={14} /> : <CheckCircle2 size={14} />}
        <span>
          {message || (status.connected
            ? `设备已连接 Wi-Fi${status.ip ? `：${status.ip}` : ""}${status.rssi ? `（信号 ${status.rssi} dBm）` : ""}`
            : "设备当前未连接 Wi-Fi。请通过 USB 扫描并配置 2.4GHz 网络。")}
        </span>
      </div>

      <div className="wifi-settings__grid">
        <section className="wifi-settings__card">
          <header className="wifi-settings__head">
            <span className="wifi-settings__icon" aria-hidden="true"><Wifi size={17} /></span>
            <div className="wifi-settings__heading">
              <strong>设备 Wi-Fi</strong>
              <span>仅支持 2.4GHz；通过 USB 下发配置，连接后设备会启动 TCP 数据通道。</span>
            </div>
          </header>
          <div className="wifi-settings__body">
            <div className="wifi-settings__fields">
              <label className="wifi-settings__field" htmlFor="wifi-ssid">
                <span>SSID</span>
                <input
                  id="wifi-ssid"
                  className="ui-control"
                  list="wifi-ssid-list"
                  value={ssid}
                  onChange={(event) => setSsid(event.target.value)}
                  placeholder="选择或输入 2.4GHz SSID"
                  disabled={Boolean(busy)}
                />
                <datalist id="wifi-ssid-list">
                  {aps.map((ap) => (
                    <option key={`${ap.ssid}-${ap.channel}`} value={ap.ssid}>
                      {`${ap.ssid}  ${ap.rssi} dBm  ch${ap.channel}`}
                    </option>
                  ))}
                </datalist>
              </label>
              <label className="wifi-settings__field" htmlFor="wifi-password">
                <span>密码</span>
                <input
                  id="wifi-password"
                  className="ui-control"
                  type="password"
                  autoComplete="new-password"
                  value={password}
                  onChange={(event) => setPassword(event.target.value)}
                  placeholder="只发送给设备，不在电脑上保存"
                  disabled={Boolean(busy)}
                />
              </label>
            </div>
            <div className="wifi-settings__actions">
              <button type="button" className="btn-secondary btn-sm" onClick={scan} disabled={Boolean(busy)}>
                {busy === "scan" ? <Loader2 size={13} className="spin" /> : <RefreshCw size={13} />} 扫描 Wi-Fi
              </button>
              <button type="button" className="btn-primary btn-sm" onClick={configure} disabled={Boolean(busy)}>
                {busy === "config" ? <Loader2 size={13} className="spin" /> : <Wifi size={13} />} 保存并连接
              </button>
              <button type="button" className="btn-secondary btn-sm" onClick={refresh} disabled={Boolean(busy)}>
                {busy === "status" ? <Loader2 size={13} className="spin" /> : <RefreshCw size={13} />} 刷新状态
              </button>
              <button type="button" className="btn-secondary btn-sm" onClick={reset} disabled={Boolean(busy)}>
                {busy === "reset" ? <Loader2 size={13} className="spin" /> : null} 清除凭据
              </button>
            </div>
          </div>
        </section>

        <div className="wifi-settings__side">
          <section className="wifi-settings__card">
            <header className="wifi-settings__head">
              <span className="wifi-settings__icon wifi-settings__icon--status" aria-hidden="true"><Activity size={16} /></span>
              <div className="wifi-settings__heading">
                <strong>设备连接</strong>
                <span>来自设备 wifi/status 的实时结果</span>
              </div>
            </header>
            <dl className="wifi-settings__facts">
              <div className="wifi-settings__fact">
                <dt>Wi-Fi 状态</dt>
                <dd className={status.connected ? "is-online" : "is-offline"}>{status.connected ? "已连接" : "未连接"}</dd>
              </div>
              <div className="wifi-settings__fact">
                <dt>设备 IP</dt>
                <dd>{status.ip || "—"}</dd>
              </div>
              <div className="wifi-settings__fact">
                <dt>信号</dt>
                <dd>{status.rssi ? `${status.rssi} dBm` : "—"}</dd>
              </div>
              <div className="wifi-settings__fact">
                <dt>TCP 客户端</dt>
                <dd className={status.clientConnected ? "is-online" : ""}>{status.clientConnected ? "已连接" : "未连接"}</dd>
              </div>
            </dl>
          </section>

          <section className="wifi-settings__card wifi-settings__card--warning">
            <header className="wifi-settings__head">
              <span className="wifi-settings__icon wifi-settings__icon--warning" aria-hidden="true"><AlertCircle size={16} /></span>
              <div className="wifi-settings__heading">
                <strong>Wi-Fi TCP 通道暂不可用</strong>
                <span>
                  {status.c6Version && status.c6Version !== "unknown"
                    ? `C6 固件：${status.c6Version}`
                    : "C6 固件版本未知"}
                  {status.wifiTcpCompatible
                    ? " · 版本已匹配，待 TCP 验证"
                    : " · 需要先升级到 2.12.11"}
                </span>
              </div>
            </header>
            <p className="wifi-settings__note wifi-settings__note--warning">
              当前检测：C6 版本 {status.c6Version || "unknown"}，
              {status.wifiTcpCompatible ? "与 Host 2.12.11 兼容。" : "与 Host esp_hosted 2.12.11 不兼容。"}
              一旦有 TCP 连接/数据，旧固件会导致 SDIO 失败并触发设备重启，表现为屏幕蓝屏与正常 UI 循环。
              Wi-Fi 配网和状态读取仍可通过 USB 正常使用；升级 C6 后才能启用数据通道。
            </p>
          </section>
        </div>
      </div>
    </div>
  );
}
