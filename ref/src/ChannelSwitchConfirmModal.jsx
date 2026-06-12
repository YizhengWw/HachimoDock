/**
 * [Input] Current and next desktop-pet channel labels plus modal actions.
 * [Output] Confirmation dialog shown before switching the single followed device subject.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React from "react";
import { AlertCircle, X } from "lucide-react";

export default function ChannelSwitchConfirmModal({
  currentLabel,
  nextLabel,
  onCancel,
  onConfirm,
}) {
  return (
    <div className="modal-backdrop" onClick={onCancel}>
      <div className="modal-card channel-switch-confirm-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-header">
          <div>
            <h3 className="modal-title">切换设备跟随主体</h3>
            <div className="modal-subtitle">设备端当前只能跟随一个本地编程工具。</div>
          </div>
          <button className="icon-btn" onClick={onCancel} aria-label="关闭">
            <X size={16} />
          </button>
        </div>
        <div className="modal-body channel-switch-confirm-modal__body">
          <div className="message-banner message-banner--muted channel-switch-confirm-modal__message">
            <AlertCircle size={14} />
            当前设备正在跟随 {currentLabel || "当前主体"}。继续后，设备端跟随主体会从 {currentLabel || "当前主体"} 切换到 {nextLabel || "目标主体"}。
          </div>
        </div>
        <div className="channel-switch-confirm-modal__actions">
          <button className="btn-secondary" type="button" onClick={onCancel}>
            取消
          </button>
          <button className="btn-primary" type="button" onClick={onConfirm}>
            继续切换
          </button>
        </div>
      </div>
    </div>
  );
}
