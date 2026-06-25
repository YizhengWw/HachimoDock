/**
 * [Input] useDeviceContext for appearances/agentAppearanceMap/agentOptions/currentDisplay/deviceConnected/appearanceSync/applyDesktopPet/saveAgentAppearance; useToast for notices.
 * [Output] Dashboard "Agent与形象" matrix: detected local Agents only, each with an independent appearance selection, one currently followed Agent synced to the device, and non-blocking inline USB appearance-sync progress that survives dashboard tab unmounts.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React, { useCallback, useMemo, useState } from "react";
import {
  CheckCircle,
  ChevronDown,
  Code,
  Loader,
  Terminal,
  UploadCloud,
  X,
  Zap,
} from "lucide-react";
import {
  appearanceById,
  channelLabelForId,
} from "../lib/agent-appearance-config.js";
import { BUILTIN_TERRIER_APPEARANCE_ID } from "../lib/builtin-appearances.js";
import {
  APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE,
  CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE,
} from "../lib/desktop-pet-assignment.js";
import AppearancePreview from "../AppearancePreview.jsx";
import { resolveDashboardPreviewMedia } from "../lib/appearance-preview.js";
import { useDeviceContext } from "../shell/DeviceContext.jsx";
import { useToast } from "../shell/ToastStack.jsx";

const AGENT_ICONS = {
  "claude-code": Code,
  codex: Terminal,
  openclaw: Zap,
};

function appearanceKindLabel(appearance) {
  if (appearance?.type === "codex-import") return "Codex pet";
  if (appearance?.type === "builtin") return "内置";
  return "自定义";
}

function normalizeSyncProgress(progress = {}) {
  const bytesSent = Number(progress.bytesSent || 0);
  const bytesTotal = Number(progress.bytesTotal || 0);
  const rawPercent = Number(progress.percent);
  const percent = Number.isFinite(rawPercent)
    ? rawPercent
    : bytesTotal > 0
      ? Math.round((bytesSent / bytesTotal) * 100)
      : 0;
  return {
    text: progress.text || "正在通过 USB 下发形象素材...",
    currentFile: Number(progress.currentFile || 0),
    totalFiles: Number(progress.totalFiles || 0),
    bytesSent,
    bytesTotal,
    percent: Math.max(0, Math.min(100, Math.round(percent))),
  };
}

export default function ChannelMatrixCard() {
  const {
    appearances,
    agentAppearanceMap,
    agentOptions,
    currentDisplay,
    deviceConnected,
    appearanceSync,
    applyDesktopPet,
    saveAgentAppearance,
  } = useDeviceContext();
  const { push } = useToast();

  const [pickerState, setPickerState] = useState(null); // { agentId }
  const [pendingFollow, setPendingFollow] = useState(null); // { agentId, appearance }
  const syncing = appearanceSync?.pending === true;
  const syncProgress = appearanceSync?.progress
    ? normalizeSyncProgress(appearanceSync.progress)
    : null;

  const installedAgents = useMemo(
    () => agentOptions.filter((agent) => agent.detected),
    [agentOptions],
  );
  const activeAgentId = currentDisplay.agentId;

  const appearanceForAgent = useCallback(
    (agentId) => {
      const appearanceId = agentAppearanceMap[agentId] || BUILTIN_TERRIER_APPEARANCE_ID;
      return appearanceById(appearances, appearanceId) || appearanceById(appearances, BUILTIN_TERRIER_APPEARANCE_ID) || appearances[0] || null;
    },
    [agentAppearanceMap, appearances],
  );

  const closePicker = useCallback(() => setPickerState(null), []);

  const handleConfirmAppearance = useCallback(async (agentId, appearance) => {
    if (!agentId || !appearance?.id) return;
    const isFollowed = agentId === activeAgentId;
    closePicker();

    if (!isFollowed) {
      saveAgentAppearance(agentId, appearance.id);
      push({
        tone: "success",
        title: `已为 ${channelLabelForId(agentOptions, agentId)} 保存「${appearance.name}」`,
      });
      return;
    }

    try {
      const { notice } = await applyDesktopPet(agentId, appearance, {
        initialProgress: {
          text: `准备下发「${appearance.name}」到设备端...`,
          percent: 0,
        },
      });
      push({ tone: "success", title: notice || `已同步「${appearance.name}」到设备端` });
    } catch (err) {
      const msg = err?.message || String(err);
      const tone = msg === APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE ? "warning" : "error";
      push({ tone, title: "更换形象失败", message: msg });
    }
  }, [activeAgentId, agentOptions, applyDesktopPet, closePicker, push, saveAgentAppearance]);

  const requestFollow = useCallback((agentId) => {
    const appearance = appearanceForAgent(agentId);
    if (!appearance) return;
    setPendingFollow({ agentId, appearance });
  }, [appearanceForAgent]);

  const confirmFollow = useCallback(async () => {
    if (!pendingFollow) return;
    const { agentId, appearance } = pendingFollow;
    setPendingFollow(null);
    try {
      const { notice } = await applyDesktopPet(agentId, appearance, {
        initialProgress: {
          text: `准备下发「${appearance.name}」并切换跟随...`,
          percent: 0,
        },
      });
      push({ tone: "success", title: notice || `已跟随 ${channelLabelForId(agentOptions, agentId)}` });
    } catch (err) {
      const msg = err?.message || String(err);
      const tone =
        msg === APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE ||
        msg === CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE
          ? "warning"
          : "error";
      push({ tone, title: "切换跟随失败", message: msg });
    }
  }, [agentOptions, applyDesktopPet, pendingFollow, push]);

  return (
    <div className="channel-matrix">
      <p className="channel-matrix__intro">
        选择设备端需要展示实时状态的Agent，每个Agent可分别设置自己的形象；
      </p>

      {syncProgress && (
        <div className="channel-matrix-sync" aria-live="polite">
          <div className="channel-matrix-sync__copy">
            <UploadCloud size={15} />
            <div>
              <strong>形象素材下发中</strong>
              <span>{syncProgress.text}</span>
            </div>
          </div>
          <div className="channel-matrix-sync__meter">
            <span>{syncProgress.percent}%</span>
            <div
              className="channel-matrix-sync__bar"
              role="progressbar"
              aria-label="形象素材下发进度"
              aria-valuemin={0}
              aria-valuemax={100}
              aria-valuenow={syncProgress.percent}
            >
              <span style={{ "--sync-progress": `${syncProgress.percent}%` }} />
            </div>
          </div>
        </div>
      )}

      {installedAgents.length === 0 ? (
        <div className="channel-matrix__empty">未扫描到本机已安装的 Agent。</div>
      ) : (
        <div className="channel-matrix__rows">
          {installedAgents.map((agent) => {
            const Icon = AGENT_ICONS[agent.id] || Code;
            const appearance = appearanceForAgent(agent.id);
            const isFollowed = activeAgentId === agent.id;
            return (
              <article
                key={agent.id}
                className={`channel-row${isFollowed ? " is-active" : ""}`}
                aria-current={isFollowed ? "true" : undefined}
              >
                <div className="channel-row__channel">
                  <span className="channel-row__icon"><Icon size={16} /></span>
                  <div className="channel-row__label">
                    <strong>{agent.label}</strong>
                  </div>
                </div>

                <div className="channel-row__formosa">
                  <div className="channel-row__thumb-wrap">
                    <AppearancePreview
                      media={resolveDashboardPreviewMedia(appearance)}
                      className="channel-row__thumb"
                      emptyClassName="channel-row__thumb channel-row__thumb--empty"
                      playing={false}
                    />
                  </div>
                  <div className="channel-row__formosa-meta">
                    <div className="channel-row__formosa-title">
                      <strong>{appearance?.name || "西高地小狗"}</strong>
                      <button
                        type="button"
                        className="btn-secondary btn-sm channel-row__change-inline"
                        onClick={() => setPickerState({ agentId: agent.id })}
                        disabled={syncing}
                      >
                        <ChevronDown size={14} /> 更换形象
                      </button>
                    </div>
                    <span>{appearanceKindLabel(appearance)}</span>
                  </div>
                </div>

                <div className="channel-row__follow">
                  {isFollowed ? (
                    <span className="channel-row__follow-current">已跟随</span>
                  ) : (
                    <button
                      type="button"
                      className="btn-primary btn-sm channel-row__follow-button"
                      onClick={() => requestFollow(agent.id)}
                      disabled={syncing || !deviceConnected}
                      title={!deviceConnected ? "设备离线，无法切换跟随" : ""}
                    >
                      跟随
                    </button>
                  )}
                </div>
              </article>
            );
          })}
        </div>
      )}

      {pickerState && (
        <AgentAppearancePickerModal
          appearances={appearances}
          agentLabel={channelLabelForId(agentOptions, pickerState.agentId)}
          selectedAppearanceId={(agentAppearanceMap[pickerState.agentId] || BUILTIN_TERRIER_APPEARANCE_ID)}
          isFollowed={pickerState.agentId === activeAgentId}
          syncing={syncing}
          onClose={closePicker}
          onConfirm={(appearance) => handleConfirmAppearance(pickerState.agentId, appearance)}
        />
      )}

      {pendingFollow && (
        <FollowAgentConfirmModal
          currentLabel={channelLabelForId(agentOptions, activeAgentId)}
          nextLabel={channelLabelForId(agentOptions, pendingFollow.agentId)}
          appearanceName={pendingFollow.appearance?.name || "西高地小狗"}
          syncing={syncing}
          onCancel={() => setPendingFollow(null)}
          onConfirm={confirmFollow}
        />
      )}
    </div>
  );
}

function AgentAppearancePickerModal({
  appearances,
  agentLabel,
  selectedAppearanceId,
  isFollowed,
  syncing,
  onClose,
  onConfirm,
}) {
  const [selectedId, setSelectedId] = useState(selectedAppearanceId || BUILTIN_TERRIER_APPEARANCE_ID);
  const selected = appearanceById(appearances, selectedId);
  const canConfirm = Boolean(selected) && !syncing;

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal-card modal-card--formosa-picker" onClick={(event) => event.stopPropagation()}>
        <div className="modal-header">
          <div>
            <h3 className="modal-title">更换形象</h3>
            <div className="modal-subtitle">
              {isFollowed
                ? `确认后会把 ${agentLabel} 的新形象实时同步到设备端。`
                : `确认后只保存 ${agentLabel} 的形象选择，不会立即同步设备端。`}
            </div>
          </div>
          <button className="icon-btn" onClick={onClose} aria-label="关闭" disabled={syncing}>
            <X size={16} />
          </button>
        </div>
        <div className="modal-body">
          <div className="formosa-picker__grid">
            {appearances.map((row) => (
              <button
                type="button"
                key={row.id}
                className={`formosa-picker__option${selectedId === row.id ? " is-selected" : ""}`}
                onClick={() => setSelectedId(row.id)}
                disabled={syncing}
              >
                <div className="formosa-picker__stage" aria-hidden="true">
                <AppearancePreview
                  media={resolveDashboardPreviewMedia(row)}
                  className="formosa-picker__media"
                  emptyClassName="formosa-picker__media formosa-picker__media--empty"
                  playing={false}
                />
                </div>
                <div className="formosa-picker__copy">
                  <strong>{row.name}</strong>
                  <span>{appearanceKindLabel(row)}</span>
                </div>
                {selectedId === row.id && <CheckCircle className="formosa-picker__check" size={16} />}
              </button>
            ))}
          </div>
        </div>
        <div className="modal-footer">
          <button className="btn-secondary" type="button" onClick={onClose} disabled={syncing}>
            取消
          </button>
          <button
            className="btn-primary"
            type="button"
            onClick={() => selected && onConfirm(selected)}
            disabled={!canConfirm}
          >
            {syncing ? <Loader size={14} className="spin" /> : <UploadCloud size={14} />}
            确认
          </button>
        </div>
      </div>
    </div>
  );
}

function FollowAgentConfirmModal({
  currentLabel,
  nextLabel,
  appearanceName,
  syncing,
  onCancel,
  onConfirm,
}) {
  return (
    <div className="modal-backdrop" onClick={onCancel}>
      <div className="modal-card channel-switch-confirm-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-header">
          <div>
            <h3 className="modal-title">切换跟随 Agent</h3>
            <div className="modal-subtitle">设备端同一时间只展示一个 Agent 的实时状态。</div>
          </div>
          <button className="icon-btn" onClick={onCancel} aria-label="关闭" disabled={syncing}>
            <X size={16} />
          </button>
        </div>
        <div className="modal-body channel-switch-confirm-modal__body">
          <div className="message-banner message-banner--muted channel-switch-confirm-modal__message">
            将设备端跟随的 Agent 从 {currentLabel || "当前 Agent"} 切换为 {nextLabel || "目标 Agent"}，并同步「{appearanceName}」到设备端展示。
          </div>
        </div>
        <div className="channel-switch-confirm-modal__actions">
          <button className="btn-secondary" type="button" onClick={onCancel} disabled={syncing}>
            取消
          </button>
          <button className="btn-primary" type="button" onClick={onConfirm} disabled={syncing}>
            {syncing && <Loader size={14} className="spin" />}
            确认跟随
          </button>
        </div>
      </div>
    </div>
  );
}
