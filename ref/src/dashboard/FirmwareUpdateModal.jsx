/**
 * [Input] open/onClose props, expected board identity, current ESP32-P4
 *         firmware version, native file picker, usb_update_firmware command,
 *         and firmware progress events.
 * [Output] A/B firmware update dialog with .bin selection, acknowledged upload
 *          progress, reboot validation stages, and only reconnect-verified
 *          valid slot/version success.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React, { useEffect, useMemo, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { open as openDialog } from "@tauri-apps/plugin-dialog";
import { CheckCircle2, FileUp, LoaderCircle, X } from "lucide-react";

const INITIAL_PROGRESS = { stage: "idle", bytesSent: 0, bytesTotal: 0, percent: 0 };

const STAGE_LABELS = {
  begin: "正在创建备用固件槽",
  upload: "正在传输并逐块校验",
  verify: "正在校验镜像完整性",
  reboot: "镜像完整性通过，正在等待设备重启",
  validate: "设备已重连，正在确认新版本有效",
};

function fileNameFromPath(path) {
  return String(path || "").split(/[\\/]/).filter(Boolean).at(-1) || "";
}

function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

export default function FirmwareUpdateModal({
  open,
  onClose,
  currentFirmware = "",
  expectedBoardDeviceId,
}) {
  const [firmwarePath, setFirmwarePath] = useState("");
  const [progress, setProgress] = useState(INITIAL_PROGRESS);
  const [pending, setPending] = useState(false);
  const [result, setResult] = useState(null);
  const [error, setError] = useState("");

  useEffect(() => {
    if (!open) return undefined;
    setFirmwarePath("");
    setProgress(INITIAL_PROGRESS);
    setPending(false);
    setResult(null);
    setError("");
    let unlisten = null;
    let cancelled = false;
    listen("usb-firmware-update-progress", (event) => {
      const payload = event?.payload || {};
      setProgress({
        stage: String(payload.stage || "upload"),
        bytesSent: Number(payload.bytesSent) || 0,
        bytesTotal: Number(payload.bytesTotal) || 0,
        percent: Math.max(0, Math.min(100, Number(payload.percent) || 0)),
      });
    }).then((stop) => {
      if (cancelled) stop();
      else unlisten = stop;
    });
    return () => {
      cancelled = true;
      unlisten?.();
    };
  }, [open]);

  const selectedName = useMemo(() => fileNameFromPath(firmwarePath), [firmwarePath]);

  const selectFirmware = async () => {
    setError("");
    const selected = await openDialog({
      multiple: false,
      directory: false,
      filters: [{ name: "ESP32-P4 firmware", extensions: ["bin"] }],
    });
    if (typeof selected === "string") setFirmwarePath(selected);
  };

  const startUpdate = async () => {
    if (!firmwarePath || pending) return;
    setPending(true);
    setResult(null);
    setError("");
    setProgress({ ...INITIAL_PROGRESS, stage: "begin" });
    try {
      const nextResult = await invoke("usb_update_firmware", {
        firmwarePath,
        expectedBoardDeviceId,
      });
      setResult(nextResult);
      setProgress((current) => ({ ...current, stage: "reboot", percent: 100 }));
    } catch (updateError) {
      setError(typeof updateError === "string" ? updateError : String(updateError));
    } finally {
      setPending(false);
    }
  };

  if (!open) return null;

  const close = () => {
    if (!pending) onClose?.();
  };
  const stageLabel = STAGE_LABELS[progress.stage] || "等待选择固件";

  return (
    <div className="modal-backdrop" role="presentation" onMouseDown={close}>
      <div
        className="modal-card firmware-update-modal"
        role="dialog"
        aria-modal="true"
        aria-label="ESP32-P4 固件升级"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <div>
            <h3 className="modal-title">ESP32-P4 固件升级</h3>
            <p className="firmware-update-modal__subtitle">
              当前版本：<code>{currentFirmware || "未知"}</code>
            </p>
          </div>
          <button type="button" className="icon-btn" onClick={close} disabled={pending} aria-label="关闭">
            <X size={16} />
          </button>
        </div>

        <div className="modal-body firmware-update-modal__body">
          <button
            type="button"
            className="firmware-update-modal__picker"
            onClick={selectFirmware}
            disabled={pending}
          >
            <FileUp size={20} />
            <span>
              <strong>{selectedName || "选择 firmware.bin"}</strong>
              <small>{firmwarePath || "仅接受 ESP-IDF 生成的 .bin 应用镜像"}</small>
            </span>
          </button>

          {(pending || result) && (
            <div className="firmware-update-modal__progress" aria-live="polite">
              <div className="firmware-update-modal__progress-meta">
                <span>{pending ? <LoaderCircle className="spin" size={15} /> : <CheckCircle2 size={15} />}</span>
                <strong>{result ? "新固件已在设备上确认有效" : stageLabel}</strong>
                <span>{progress.percent}%</span>
              </div>
              <div className="firmware-update-modal__progress-track">
                <span style={{ width: `${progress.percent}%` }} />
              </div>
              <small>{formatBytes(progress.bytesSent)} / {formatBytes(progress.bytesTotal)}</small>
            </div>
          )}

          {result && (
            <div className="firmware-update-modal__result" data-tone="success">
              设备已重连，新版本 <code>{result.version || "未知"}</code> 已在
              <code>{result.targetPartition || "备用槽"}</code> 确认为 <code>{result.imageState || "valid"}</code>。
            </div>
          )}
          {error && <div className="firmware-update-modal__result" data-tone="error">{error}</div>}
          {!pending && !result && !error && (
            <p className="firmware-update-modal__notice">
              升级写入备用槽，不会覆盖当前可启动版本。传输开始后请保持 USB 连接和设备供电。
            </p>
          )}
        </div>

        <div className="modal-footer firmware-update-modal__footer">
          <button type="button" className="btn-secondary" onClick={close} disabled={pending}>
            {result ? "完成" : "取消"}
          </button>
          {!result && (
            <button
              type="button"
              className="btn-primary"
              onClick={startUpdate}
              disabled={!firmwarePath || pending}
            >
              {pending ? "升级中..." : "开始升级"}
            </button>
          )}
        </div>
      </div>
    </div>
  );
}
