/**
 * [Input] onSendTest / onCopyDesktopId / onUnbind callbacks plus optional应急 callbacks
 *         onDeviceReturnHome / onForceSyncAppearance (opt-in：caller 传才渲染，
 *         保证既有调用方/测试不破)，以及可选 onUpdateFirmware /
 *         onDiagnostics opt-ins for P4 A/B updates and diagnostics.
 * [Output] Menu mounted into PageShell actions: 发送测试消息 / 复制桌面设备 ID /
 *          [⤴ 设备返回主屏]? / [🔄 强制同步形象]? /
 *          [opt-in] 固件升级 / 设备诊断 / 解绑设备 (danger).
 *          应急两项在物理按键失灵切不回主屏、客户端 UI 与设备形象脱节时使用。
 * [Pos] component node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import React, { useEffect, useRef, useState } from "react";
import { Activity, MoreHorizontal, Send, Copy, Unlink, ArrowUpLeft, RefreshCw, FileUp } from "lucide-react";

export default function DashboardActionsMenu({
  onSendTest,
  onCopyDesktopId,
  onUnbind,
  onDeviceReturnHome,
  onForceSyncAppearance,
  onUpdateFirmware,
  onDiagnostics,
}) {
  const [open, setOpen] = useState(false);
  const wrapRef = useRef(null);

  useEffect(() => {
    if (!open) return undefined;
    const handler = (event) => {
      if (wrapRef.current && !wrapRef.current.contains(event.target)) setOpen(false);
    };
    document.addEventListener("mousedown", handler);
    return () => document.removeEventListener("mousedown", handler);
  }, [open]);

  const dispatch = (cb) => () => {
    setOpen(false);
    cb?.();
  };

  return (
    <div className="dashboard-actions-menu" ref={wrapRef}>
      <button
        type="button"
        className="icon-btn dashboard-actions-menu__trigger"
        onClick={() => setOpen((prev) => !prev)}
        aria-haspopup="menu"
        aria-expanded={open}
        title="更多操作"
      >
        <MoreHorizontal size={16} />
      </button>
      {open && (
        <div className="dashboard-actions-menu__list" role="menu">
          <button
            type="button"
            className="dashboard-actions-menu__item"
            role="menuitem"
            onClick={dispatch(onSendTest)}
          >
            <Send size={14} />
            发送测试消息
          </button>
          <button
            type="button"
            className="dashboard-actions-menu__item"
            role="menuitem"
            onClick={dispatch(onCopyDesktopId)}
          >
            <Copy size={14} />
            复制桌面设备 ID
          </button>
          {onDeviceReturnHome && (
            <button
              type="button"
              className="dashboard-actions-menu__item"
              role="menuitem"
              onClick={dispatch(onDeviceReturnHome)}
              title="负一屏物理按键切不回主屏时，通过 USB 强制把设备 .screen-page 切到 main"
            >
              <ArrowUpLeft size={14} />
              ⤴ 设备返回主屏
            </button>
          )}
          {onForceSyncAppearance && (
            <button
              type="button"
              className="dashboard-actions-menu__item"
              role="menuitem"
              onClick={dispatch(onForceSyncAppearance)}
              title="客户端 UI 显示的形象与设备屏幕不一致时，绕过缓存重新推送一次"
            >
              <RefreshCw size={14} />
              🔄 强制同步形象
            </button>
          )}
          {onUpdateFirmware && (
            <button
              type="button"
              className="dashboard-actions-menu__item"
              role="menuitem"
              onClick={dispatch(onUpdateFirmware)}
            >
              <FileUp size={14} />
              升级 ESP32-P4 固件
            </button>
          )}
          {onDiagnostics && (
            <button
              type="button"
              className="dashboard-actions-menu__item"
              role="menuitem"
              onClick={dispatch(onDiagnostics)}
            >
              <Activity size={14} />
              ESP32-P4 设备诊断
            </button>
          )}
          <button
            type="button"
            className="dashboard-actions-menu__item dashboard-actions-menu__item--danger"
            role="menuitem"
            onClick={dispatch(onUnbind)}
          >
            <Unlink size={14} />
            解绑设备
          </button>
        </div>
      )}
    </div>
  );
}
