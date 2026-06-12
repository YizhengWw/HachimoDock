/**
 * [Input] open/onClose props + Tauri invoke for usb_apply_wifi + listen for usb-message.
 * [Output] Shared modal-card dialog with SSID/password inputs and a live
 *          three-stage indicator (applying / connected with IP / failed with
 *          code) driven by the device-side apply-wifi-ack frames.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { X } from "lucide-react";

const STAGE_INITIAL = "initial";
const STAGE_APPLYING = "applying";
const STAGE_CONNECTED = "connected";
const STAGE_FAILED = "failed";

const ERROR_HINT = {
  invalid_json: "请求格式错误",
  invalid_ssid: "SSID 含有非法字符或长度超限",
  invalid_psk: "密码含有非法字符或长度超限",
  already_in_progress: "已有一次配网在进行中",
  sta_apply_unconfigured: "板端未启用 STA 切换脚本",
  ssid_not_found: "找不到该 SSID（是否拼错或不在范围内）",
  wrong_password_or_assoc: "密码错误或关联失败",
  no_dhcp_lease: "已关联但未拿到 IP（路由器 DHCP 问题？）",
  timeout: "超时未拿到结果",
  spawn_failed: "板端无法启动 STA 切换脚本",
  thread_failed: "板端无法创建工作线程",
  oom: "板端内存不足",
};

export default function WifiApplyModal({ open, onClose }) {
  const [ssid, setSsid] = useState("");
  const [psk, setPsk] = useState("");
  const [stage, setStage] = useState(STAGE_INITIAL);
  const [ip, setIp] = useState("");
  const [error, setError] = useState("");
  const submittingRef = useRef(false);

  useEffect(() => {
    if (open) {
      setSsid("");
      setPsk("");
      setStage(STAGE_INITIAL);
      setIp("");
      setError("");
      submittingRef.current = false;
    }
  }, [open]);

  useEffect(() => {
    if (!open) return undefined;
    let unlisten = null;
    let cancelled = false;
    (async () => {
      const fn = await listen("usb-message", (event) => {
        const data = event && event.payload;
        if (!data || data.topic !== "apply-wifi-ack") return;
        const ackPayload = data.payload || {};
        const nextStage = ackPayload.stage || "";
        if (nextStage === STAGE_APPLYING) {
          setStage(STAGE_APPLYING);
          setError("");
          setIp("");
        } else if (nextStage === STAGE_CONNECTED) {
          setStage(STAGE_CONNECTED);
          setIp(ackPayload.ip || "");
          setError("");
          submittingRef.current = false;
        } else if (nextStage === STAGE_FAILED) {
          setStage(STAGE_FAILED);
          setError(ackPayload.error || "unknown");
          setIp("");
          submittingRef.current = false;
        }
      });
      if (cancelled) {
        fn();
      } else {
        unlisten = fn;
      }
    })();
    return () => {
      cancelled = true;
      if (unlisten) unlisten();
    };
  }, [open]);

  const handleSubmit = async (event) => {
    event.preventDefault();
    if (submittingRef.current) return;
    if (!ssid.trim()) {
      setError("invalid_ssid");
      setStage(STAGE_FAILED);
      return;
    }
    submittingRef.current = true;
    setStage(STAGE_APPLYING);
    setError("");
    setIp("");
    try {
      await invoke("usb_apply_wifi", { ssid: ssid.trim(), psk });
    } catch (err) {
      submittingRef.current = false;
      setStage(STAGE_FAILED);
      setError(typeof err === "string" ? err : "invoke_failed");
    }
  };

  if (!open) return null;

  return (
    <div className="modal-backdrop" role="presentation" onMouseDown={onClose}>
      <div
        className="modal-card wifi-apply-modal"
        role="dialog"
        aria-modal="true"
        aria-label="通过 USB 配 WiFi"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <div>
            <h3 className="modal-title">通过 USB 配 WiFi</h3>
          </div>
          <button type="button" className="icon-btn" onClick={onClose} aria-label="关闭">
            <X size={16} />
          </button>
        </div>
        <form className="wifi-apply-modal__form" onSubmit={handleSubmit}>
          <div className="modal-body wifi-apply-modal__body">
            <label className="ui-field" htmlFor="wifi-apply-ssid">
              <span className="ui-field__label">SSID</span>
              <input
                id="wifi-apply-ssid"
                className="ui-control"
                name="ssid"
                type="text"
                value={ssid}
                maxLength={64}
                onChange={(event) => setSsid(event.target.value)}
                autoComplete="off"
                placeholder="例如：HomeWiFi-2G"
                disabled={stage === STAGE_APPLYING}
                required
              />
            </label>
            <label className="ui-field" htmlFor="wifi-apply-psk">
              <span className="ui-field__label">密码</span>
              <input
                id="wifi-apply-psk"
                className="ui-control"
                name="psk"
                type="password"
                value={psk}
                maxLength={64}
                onChange={(event) => setPsk(event.target.value)}
                autoComplete="off"
                placeholder="开放网络可留空"
                disabled={stage === STAGE_APPLYING}
              />
            </label>
            <div className="wifi-apply-modal__stage" data-stage={stage}>
              {stage === STAGE_APPLYING && <span>应用中... 板端正在切换 WiFi，最长 25 秒</span>}
              {stage === STAGE_CONNECTED && (
                <span>已连接 ✅ 板端 IP: <code>{ip || "(未知)"}</code></span>
              )}
              {stage === STAGE_FAILED && (
                <span>失败 ❌ {ERROR_HINT[error] || error || "未知错误"}</span>
              )}
            </div>
          </div>
          <div className="modal-footer">
            <button type="button" className="btn-secondary" onClick={onClose}>关闭</button>
            <button
              type="submit"
              className="btn-primary"
              disabled={stage === STAGE_APPLYING || !ssid.trim()}
            >
              {stage === STAGE_APPLYING ? "应用中..." : "提交"}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
