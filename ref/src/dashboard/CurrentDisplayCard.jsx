/**
 * [Input] useDeviceContext for binding/usb/appearances/agentAppearanceMap/agentOptions/currentDisplay/applyDesktopPet; useToast for notices.
 * [Output] Region 2 of the device dashboard: large preview + 渠道 dropdown (independent) + 「更换 ▾」 formosa picker; routes per spec § 渠道与形象的独立切换.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React, { useCallback, useState } from "react";
import { ChevronDown, ImagePlus, Loader, UploadCloud, X, CheckCircle } from "lucide-react";
import AppearancePreview from "../AppearancePreview.jsx";
import ChannelSwitchConfirmModal from "../ChannelSwitchConfirmModal.jsx";
import { resolveDashboardPreviewMedia } from "../lib/appearance-preview.js";
import {
  appearanceById,
  channelLabelForId,
  shouldConfirmChannelSwitch,
} from "../lib/agent-appearance-config.js";
import {
  APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE,
  CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE,
} from "../lib/desktop-pet-assignment.js";
import { useDeviceContext } from "../shell/DeviceContext.jsx";
import { useToast } from "../shell/ToastStack.jsx";

export default function CurrentDisplayCard() {
  const {
    usb,
    deviceOnline,
    appearances,
    agentAppearanceMap,
    agentOptions,
    currentDisplay,
    applyDesktopPet,
  } = useDeviceContext();
  const { push } = useToast();

  const [pickerOpen, setPickerOpen] = useState(false);
  const [pendingChannel, setPendingChannel] = useState(null);
  const [syncing, setSyncing] = useState(false);

  const currentAppearance = currentDisplay.appearance;
  const previewMedia = resolveDashboardPreviewMedia(currentAppearance);
  const currentAgentId = currentDisplay.agentId;
  const detectedAgents = agentOptions.filter((a) => a.detected || a.id === currentAgentId);

  // --- Channel input -------------------------------------------------------
  const performChannelChange = useCallback(
    async (nextAgentId) => {
      const nextAppearanceId =
        agentAppearanceMap[nextAgentId] || currentDisplay.appearance?.id || "";
      const nextAppearance = appearanceById(appearances, nextAppearanceId);
      if (!nextAppearance) {
        push({ tone: "warning", title: "请选择一个形象后再切换渠道" });
        return;
      }
      setSyncing(true);
      try {
        const { notice } = await applyDesktopPet(nextAgentId, nextAppearance, {
          onProgress: (p) => push({ tone: "info", title: p.text, ttl: 2000 }),
        });
        push({ tone: "success", title: notice });
      } catch (err) {
        const msg = err?.message || String(err);
        const tone =
          msg === APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE ||
          msg === CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE
            ? "warning"
            : "error";
        push({ tone, title: msg });
      } finally {
        setSyncing(false);
      }
    },
    [agentAppearanceMap, appearances, applyDesktopPet, currentDisplay, push],
  );

  const handleChannelChange = useCallback(
    (nextAgentId) => {
      if (!nextAgentId || nextAgentId === currentAgentId) return;
      // Row 3: if the target agent has no historical formosa, reuse the current
      // appearance and skip the confirm modal. applyDesktopPet writes the
      // fallback into agentAppearanceMap via assignAppearanceToAgent so future
      // switches see it.
      if (!agentAppearanceMap[nextAgentId]) {
        performChannelChange(nextAgentId);
        return;
      }
      // Existing path: rows 1 + 2 — fire confirm modal.
      if (shouldConfirmChannelSwitch(agentAppearanceMap, nextAgentId, new Set([currentAgentId]))) {
        setPendingChannel(nextAgentId);
        return;
      }
      performChannelChange(nextAgentId);
    },
    [agentAppearanceMap, currentAgentId, performChannelChange],
  );

  const confirmPendingChannel = useCallback(() => {
    if (pendingChannel) performChannelChange(pendingChannel);
    setPendingChannel(null);
  }, [pendingChannel, performChannelChange]);

  // --- Formosa input (independent) -----------------------------------------
  const performFormosaChange = useCallback(
    async (nextAppearanceId) => {
      const nextAppearance = appearanceById(appearances, nextAppearanceId);
      if (!nextAppearance || !currentAgentId) return;
      setSyncing(true);
      try {
        const { notice } = await applyDesktopPet(currentAgentId, nextAppearance, {
          onProgress: (p) => push({ tone: "info", title: p.text, ttl: 2000 }),
        });
        push({ tone: "success", title: notice });
        setPickerOpen(false);
      } catch (err) {
        const msg = err?.message || String(err);
        const tone = msg === APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE ? "warning" : "error";
        push({ tone, title: msg });
      } finally {
        setSyncing(false);
      }
    },
    [appearances, applyDesktopPet, currentAgentId, push],
  );

  // Spec row 4: formosa change requires USB.
  const formosaCanApply = Boolean(usb.connected);
  const formosaGateMessage = formosaCanApply ? "" : APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE;

  return (
    <div className="dashboard-current-display">
      <div className="dashboard-current-display__preview">
        {currentAppearance ? (
          <>
            <span className="appearance-thumb__badge">
              {currentAppearance.type === "codex-import"
                ? "codex pet"
                : currentAppearance.type === "builtin"
                  ? "内置形象"
                  : "自定义形象"}
            </span>
            <AppearancePreview
              media={previewMedia}
              className="dashboard-current-display__preview-media"
              emptyClassName="dashboard-current-display__preview-empty"
              playing
            />
          </>
        ) : (
          <div className="dashboard-current-display__preview-empty">
            <ImagePlus size={20} />
          </div>
        )}
      </div>

      <div className="dashboard-current-display__inputs">
        <label className="dashboard-current-display__channel">
          <span>渠道</span>
          <span className="dashboard-current-display__select-shell">
            <select
              className="dashboard-current-display__channel-select"
              value={currentAgentId}
              onChange={(event) => handleChannelChange(event.target.value)}
              disabled={syncing}
            >
              {detectedAgents.map((agent) => (
                <option key={agent.id} value={agent.id}>
                  {agent.label}
                </option>
              ))}
            </select>
            <ChevronDown size={14} aria-hidden="true" />
          </span>
        </label>

        <div className="dashboard-current-display__formosa">
          <span>形象</span>
          <div className="dashboard-current-display__formosa-row">
            <strong>{currentAppearance?.name || "未选择形象"}</strong>
            <button
              type="button"
              className="btn-secondary btn-sm"
              onClick={() => setPickerOpen(true)}
              disabled={syncing}
            >
              更换
              <ChevronDown size={14} aria-hidden="true" />
            </button>
          </div>
          {!usb.connected && (
            <p className="dashboard-current-display__hint">
              更换形象需要 USB 直连设备。当前{deviceOnline ? "在线（仅 WiFi）" : "离线"}。
            </p>
          )}
        </div>
      </div>

      {pickerOpen && (
        <AgentAppearancePickerModal
          appearances={appearances}
          selectedAppearanceId={currentAppearance?.id || ""}
          syncing={syncing}
          deviceConnected={formosaCanApply}
          deviceConnectionMessage={formosaGateMessage}
          onClose={() => setPickerOpen(false)}
          onPick={(appearanceId) => performFormosaChange(appearanceId)}
        />
      )}

      {pendingChannel && (
        <ChannelSwitchConfirmModal
          currentLabel={channelLabelForId(agentOptions, currentAgentId)}
          nextLabel={channelLabelForId(agentOptions, pendingChannel)}
          onCancel={() => setPendingChannel(null)}
          onConfirm={confirmPendingChannel}
        />
      )}
    </div>
  );
}

function AgentAppearancePickerModal({
  appearances,
  selectedAppearanceId,
  syncing,
  deviceConnected,
  deviceConnectionMessage,
  onPick,
  onClose,
}) {
  const [draftId, setDraftId] = useState(selectedAppearanceId || "");
  const canApply = !syncing && draftId && deviceConnected;
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div
        className="modal-card modal-card--appearance-picker"
        onClick={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <div>
            <h3 className="modal-title">选择设备展示形象</h3>
            <div className="modal-subtitle">仅替换当前渠道下的形象，不变更渠道。</div>
          </div>
          <button className="icon-btn" onClick={onClose} aria-label="关闭">
            <X size={16} />
          </button>
        </div>
        <div className="modal-body agent-appearance-picker-modal__body">
          <div className="agent-appearance-picker-modal__list">
            {appearances.map((row) => (
              <AgentAppearancePickerOption
                key={row.id}
                row={row}
                selected={draftId === row.id}
                disabled={syncing}
                onPick={() => setDraftId(row.id)}
              />
            ))}
          </div>
          {!deviceConnected && (
            <div className="message-banner message-banner--warning agent-appearance-picker-modal__notice">
              {deviceConnectionMessage}
            </div>
          )}
        </div>
        <div className="agent-appearance-picker-modal__actions">
          <button className="btn-secondary" type="button" onClick={onClose} disabled={syncing}>
            取消
          </button>
          <button
            className="btn-primary"
            type="button"
            onClick={() => canApply && onPick(draftId)}
            disabled={!canApply}
            title={!deviceConnected ? deviceConnectionMessage : undefined}
          >
            {syncing ? <Loader size={14} className="spin" /> : <UploadCloud size={14} />}
            {syncing ? "应用中…" : "设为桌宠"}
          </button>
        </div>
      </div>
    </div>
  );
}

function AgentAppearancePickerOption({ row, selected, disabled, onPick }) {
  const previewMedia = resolveDashboardPreviewMedia(row);
  return (
    <button
      type="button"
      className={`agent-appearance-picker-modal__option${selected ? " is-selected" : ""}`}
      onClick={onPick}
      disabled={disabled}
    >
      <span className="agent-appearance-picker-modal__preview">
        <span className="appearance-thumb__badge">
          {row.type === "codex-import"
            ? "codex pet"
            : row.type === "builtin"
              ? "内置形象"
              : "自定义形象"}
        </span>
        <AppearancePreview
          media={previewMedia}
          className="appearance-channel-preview__media"
          emptyClassName="appearance-channel-preview__empty"
        />
      </span>
      <span className="agent-appearance-picker-modal__copy">
        <strong>{row.name}</strong>
        <span>{row.description || `${row.provider} · ${row.model || "—"}`}</span>
      </span>
      {selected && <CheckCircle className="agent-appearance-picker-modal__check" size={18} />}
    </button>
  );
}
