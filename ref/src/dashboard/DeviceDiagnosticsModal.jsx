/**
 * [Input] open/onClose callbacks, expected board identity, and ACK-gated P4
 *         diagnostic Tauri commands.
 * [Output] Live reset/crash, memory, storage and runtime report with safe reboot
 *          and input-binding reset actions that preserve appearance assets and
 *          await client cache rehydration from the board.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React, { useCallback, useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { AlertTriangle, LoaderCircle, Power, RefreshCw, RotateCcw, X } from "lucide-react";

function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function formatDuration(value) {
  const seconds = Math.max(0, Math.floor((Number(value) || 0) / 1000));
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const rest = seconds % 60;
  if (hours) return `${hours} 小时 ${minutes} 分`;
  if (minutes) return `${minutes} 分 ${rest} 秒`;
  return `${rest} 秒`;
}

const RESET_LABELS = {
  power_on: "上电启动",
  external_pin: "外部复位",
  software: "软件重启",
  panic: "程序异常",
  interrupt_watchdog: "中断看门狗",
  task_watchdog: "任务看门狗",
  watchdog: "看门狗",
  brownout: "供电欠压",
  power_glitch: "电源抖动",
  cpu_lockup: "CPU 锁死",
  usb: "USB 复位",
  jtag: "JTAG 复位",
};

function Metric({ label, value, tone = "" }) {
  return (
    <div className="device-diagnostics-modal__metric" data-tone={tone || undefined}>
      <dt>{label}</dt>
      <dd>{value}</dd>
    </div>
  );
}

export default function DeviceDiagnosticsModal({
  open,
  onClose,
  onInputConfigReset,
  expectedBoardDeviceId,
}) {
  const [report, setReport] = useState(null);
  const [pending, setPending] = useState(false);
  const [action, setAction] = useState("");
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const [confirmAction, setConfirmAction] = useState("");

  const loadDiagnostics = useCallback(async () => {
    setPending(true);
    setError("");
    try {
      setReport(await invoke("usb_get_diagnostics", { expectedBoardDeviceId }));
    } catch (loadError) {
      setError(typeof loadError === "string" ? loadError : String(loadError));
    } finally {
      setPending(false);
    }
  }, [expectedBoardDeviceId]);

  useEffect(() => {
    if (!open) return;
    setReport(null);
    setAction("");
    setError("");
    setNotice("");
    setConfirmAction("");
    loadDiagnostics();
  }, [loadDiagnostics, open]);

  const resetInputs = async () => {
    if (confirmAction !== "reset-inputs") {
      setConfirmAction("reset-inputs");
      return;
    }
    setAction("reset-inputs");
    setError("");
    setNotice("");
    try {
      const result = await invoke("usb_reset_input_config", { expectedBoardDeviceId });
      setNotice(result?.message || "按键和旋钮已恢复默认映射，形象素材未改动。");
      setConfirmAction("");
      await onInputConfigReset?.();
      await loadDiagnostics();
    } catch (resetError) {
      setError(typeof resetError === "string" ? resetError : String(resetError));
    } finally {
      setAction("");
    }
  };

  const reboot = async () => {
    if (confirmAction !== "reboot") {
      setConfirmAction("reboot");
      return;
    }
    setAction("reboot");
    setError("");
    try {
      const result = await invoke("usb_reboot_device", { expectedBoardDeviceId });
      setNotice(result?.message || "设备正在重启，USB 会短暂断开。");
      setConfirmAction("");
    } catch (rebootError) {
      setError(typeof rebootError === "string" ? rebootError : String(rebootError));
    } finally {
      setAction("");
    }
  };

  if (!open) return null;
  const runtime = report?.runtime || {};
  const memory = report?.memory || {};
  const storage = report?.storage || {};
  const desktopBuild = report?.desktopBuild || {};
  const resetReason = RESET_LABELS[report?.lastResetReason] || report?.lastResetReason || "未知";
  const storageUsage = storage.totalBytes
    ? `${formatBytes(storage.usedBytes)} / ${formatBytes(storage.totalBytes)}`
    : "未知";

  return (
    <div className="modal-backdrop" role="presentation" onMouseDown={() => onClose?.()}>
      <div
        className="modal-card device-diagnostics-modal"
        role="dialog"
        aria-modal="true"
        aria-label="ESP32-P4 设备诊断"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <div>
            <h3 className="modal-title">设备诊断</h3>
            <p className="device-diagnostics-modal__subtitle">ESP32-P4 实时运行状态与恢复工具</p>
          </div>
          <button type="button" className="icon-btn" onClick={() => onClose?.()} aria-label="关闭">
            <X size={16} />
          </button>
        </div>

        <div className="modal-body device-diagnostics-modal__body">
          <div className="device-diagnostics-modal__toolbar">
            <span className={report?.lastResetWasFault ? "is-fault" : ""}>
              {report?.lastResetWasFault && <AlertTriangle size={14} />}
              最近复位：{resetReason}
            </span>
            <button type="button" className="icon-btn" onClick={loadDiagnostics} disabled={pending} title="刷新诊断">
              {pending ? <LoaderCircle className="spin" size={15} /> : <RefreshCw size={15} />}
            </button>
          </div>

          {report && (
            <dl className="device-diagnostics-modal__metrics">
              <Metric label="运行时间" value={formatDuration(runtime.uptimeMs)} />
              <Metric
                label="固件 / 分区 / 镜像状态"
                value={`${runtime.firmware || "未知"} / ${runtime.runningPartition || "未知"} / ${runtime.imageState || "未知"}`}
              />
              <Metric label="设备构建" value={runtime.buildId || "旧固件未提供"} />
              <Metric label="PC 构建" value={desktopBuild.buildId || "未知"} />
              <Metric label="协议 Schema（设备 / PC）" value={`${runtime.protocolSchema || "?"} / ${desktopBuild.protocolSchema || "?"}`} />
              <Metric label="启动 / 故障复位" value={`${report.bootCount || 0} / ${report.faultResetCount || 0}`} tone={report.lastResetWasFault ? "warning" : ""} />
              <Metric label="页面 / Agent" value={`${runtime.screenPage || "未知"} / ${runtime.agentState || "idle"}`} />
              <Metric label="堆内存（当前 / 最低）" value={`${formatBytes(memory.freeHeapBytes)} / ${formatBytes(memory.minimumFreeHeapBytes)}`} />
              <Metric label="PSRAM（当前 / 最低）" value={`${formatBytes(memory.freePsramBytes)} / ${formatBytes(memory.minimumFreePsramBytes)}`} />
              <Metric label="形象存储 / 槽位" value={`${storageUsage} / ${storage.activeAppearanceSlot ?? "?"}`} />
              <Metric label="任务 / 输入丢包" value={`${runtime.taskCount || 0} / ${runtime.inputDroppedEvents || 0}`} tone={runtime.inputDroppedEvents ? "warning" : ""} />
            </dl>
          )}
          {pending && !report && <div className="device-diagnostics-modal__loading"><LoaderCircle className="spin" size={18} />正在读取设备...</div>}
          {error && <div className="device-diagnostics-modal__message" data-tone="error">{error}</div>}
          {notice && <div className="device-diagnostics-modal__message" data-tone="success">{notice}</div>}

          <div className="device-diagnostics-modal__actions">
            <button type="button" className="btn-secondary" onClick={resetInputs} disabled={Boolean(action)}>
              {action === "reset-inputs" ? <LoaderCircle className="spin" size={14} /> : <RotateCcw size={14} />}
              {confirmAction === "reset-inputs" ? "再次点击确认" : "恢复默认按键"}
            </button>
            <button type="button" className="btn-secondary" onClick={reboot} disabled={Boolean(action)}>
              {action === "reboot" ? <LoaderCircle className="spin" size={14} /> : <Power size={14} />}
              {confirmAction === "reboot" ? "再次点击确认" : "重启设备"}
            </button>
          </div>
          <p className="device-diagnostics-modal__preserve">以上操作均保留设备中的形象素材与当前可回滚固件。</p>
        </div>
      </div>
    </div>
  );
}
