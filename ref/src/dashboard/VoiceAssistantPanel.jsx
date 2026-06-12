/**
 * [Input] state (busStatus/busSessions/busSessionId/voiceRuntime/audioBridge{*}/mockInject/deviceVoiceFlow/selectedAgentId/deviceOnline) + dispatch + toggleAudioBridge/sendMockButtonInject + voiceConfig + selectedTrigger + onVoiceConfigChange.
 * [Output] Region 4 of the device dashboard: lean voice panel with shared-form controls for voice on/off, 续接会话, mock text injection, board voice action status, and 启动/停止板端收听. Renders inside <Card.Collapsible>; the parent owns the wrapper.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React from "react";
import { ChevronDown, Loader, Mic, MicOff, Send } from "lucide-react";

export function buildVoiceSummary(voiceConfig, selectedTrigger) {
  if (!voiceConfig?.enabled) return "未开启";
  const trigger = selectedTrigger?.label || "默认触发";
  return `已开启 · ${trigger}`;
}

export function formatVoiceSessionOption(session) {
  if (!session || typeof session !== "object") return "未知会话";
  const parts = [];
  const name = typeof session.name === "string" ? session.name.trim() : "";
  if (name) parts.push(name);

  const startedAt = Number(session.createdAt) > 0
    ? Number(session.createdAt)
    : Number(session.lastModified);
  const ts = startedAt > 0
    ? new Date(startedAt).toLocaleString()
    : "";
  if (ts) parts.push(ts);

  const cwdName = basenameFromPath(session.cwd);
  if (cwdName && !name) parts.push(cwdName);

  const summary = typeof session.summary === "string" ? session.summary.trim() : "";
  const shortId = typeof session.id === "string" && session.id
    ? session.id.slice(0, 8)
    : "";
  if (!name && summary) {
    parts.push(summary);
  } else if (!name && shortId) {
    parts.push(`会话 ${shortId}`);
  }

  return parts.length ? parts.join(" · ") : "未知会话";
}

function basenameFromPath(value) {
  if (typeof value !== "string") return "";
  const normalized = value.trim().replace(/[\\/]+$/, "");
  if (!normalized) return "";
  const parts = normalized.split(/[\\/]+/);
  return parts[parts.length - 1] || "";
}

export default function VoiceAssistantPanel({
  state,
  dispatch,
  toggleAudioBridge,
  sendMockButtonInject,
  voiceConfig,
  selectedTrigger,
  onVoiceConfigChange,
}) {
  const agents = Array.isArray(state.busStatus?.agents) ? state.busStatus.agents : [];
  const selectedAgent = agents.find((agent) => agent.agentId === state.selectedAgentId) || null;
  const ready = selectedAgent?.ready === true;
  const sessions = Array.isArray(state.busSessions) ? state.busSessions : [];

  const voiceRunning = state.voiceRuntime?.running === true;
  let audioBlockingReason = null;
  if (!state.selectedAgentId) {
    audioBlockingReason = "请先在「当前展示」里选择一个渠道";
  } else if (state.voiceRuntime == null) {
    audioBlockingReason = "正在检查语音通道...";
  } else if (!voiceRunning) {
    audioBlockingReason = state.voiceRuntime?.message || "语音通道暂未启动（voice-service 未就绪）";
  } else if (state.busStatus == null) {
    audioBlockingReason = "正在检查语音通道...";
  } else if (state.busStatus != null && !ready) {
    audioBlockingReason = selectedAgent?.reason || "语音 agent 未就绪";
  }

  const boardOffline = state.deviceOnline === false;

  return (
    <div className="voice-panel voice-panel--compact">
      <div className="voice-panel__toolbar">
        <label
          className={`voice-config-switch voice-config-switch--inline${voiceConfig.enabled ? " is-on" : ""}`}
          title="关闭时不会启动板端麦克风收听"
        >
          <input
            type="checkbox"
            checked={voiceConfig.enabled}
            onChange={(event) => onVoiceConfigChange({ enabled: event.target.checked })}
          />
          <span className="voice-config-switch__track" aria-hidden="true">
            <span className="voice-config-switch__thumb" />
          </span>
          <span className="voice-config-switch__label">是否开启语音</span>
        </label>

        <div className="ui-field ui-field--inline voice-panel__session">
          <label className="ui-field__label" htmlFor="voice-session-select">续接会话</label>
          <span className="ui-control-shell voice-panel__session-control">
            <select
              id="voice-session-select"
              className="ui-control ui-control--select"
              value={state.busSessionId}
              disabled={!ready || sessions.length === 0}
              onChange={(event) => {
                const value = event.target.value || "auto";
                dispatch({ type: "set_bus_session_id", value });
                try {
                  if (value && value !== "auto") {
                    localStorage.setItem(
                      `pet-manager.voice-session.${state.selectedAgentId}`,
                      value,
                    );
                  } else {
                    localStorage.removeItem(`pet-manager.voice-session.${state.selectedAgentId}`);
                  }
                } catch {
                  // ignore storage errors
                }
              }}
            >
              <option value="auto">最近的会话（自动）</option>
              {sessions.map((session) => {
                const label = formatVoiceSessionOption(session);
                return (
                  <option key={session.id} value={session.id}>
                    {label}
                  </option>
                );
              })}
            </select>
            <ChevronDown className="ui-control-shell__chevron" size={14} aria-hidden="true" />
          </span>
        </div>

        <div className="voice-panel__listen">
          {state.audioBridgeEnabled ? (
            <button
              type="button"
              className="btn-primary btn-sm voice-panel__listen-btn"
              onClick={() => toggleAudioBridge("stop")}
              disabled={state.audioBridgePending}
            >
              <MicOff size={14} aria-hidden="true" />
              停止板端收听
            </button>
          ) : (
            <button
              type="button"
              className="btn-primary btn-sm voice-panel__listen-btn"
              onClick={() => toggleAudioBridge("start")}
              disabled={
                state.audioBridgePending || !!audioBlockingReason || !voiceConfig.enabled
              }
              title={
                !voiceConfig.enabled
                  ? "请先开启板端语音"
                  : audioBlockingReason || "启动前会自动检查本地 Bridge 和 voice-service。"
              }
            >
              <Mic size={14} aria-hidden="true" />
              启动板端麦克风收听
            </button>
          )}

          {state.audioBridgePending && (
            <span className="voice-inline-status">
              <Loader size={14} className="spin" />
              正在下发信令...
            </span>
          )}

          {boardOffline && !audioBlockingReason && voiceConfig.enabled && !state.audioBridgeEnabled && (
            <span className="voice-panel__status voice-panel__status--muted">
              板子当前离线，信令仍会下发并在上线后生效。
            </span>
          )}
        </div>
      </div>

      <div className="voice-panel__mock voice-panel__compose">
        <label className="ui-field voice-panel__compose-field" htmlFor="voice-mock-inject-input">
          <span className="ui-field__label">测试输入（模拟按钮语音转文字）</span>
          <textarea
            id="voice-mock-inject-input"
            className="ui-control ui-control--textarea voice-panel__mock-textarea"
            value={state.mockInjectInput || ""}
            onChange={(event) => dispatch({
              type: "set_mock_inject_input",
              value: event.target.value,
            })}
            placeholder="输入要注入当前 session 的文本"
            rows={3}
          />
        </label>
        <div className="voice-panel__mock-actions">
          <button
            type="button"
            className="btn-primary btn-sm"
            disabled={
              !ready
              || !state.selectedAgentId
              || state.mockInjectPending
              || !(state.mockInjectInput || "").trim()
            }
            onClick={sendMockButtonInject}
          >
            {state.mockInjectPending ? (
              <>
                <Loader size={14} className="spin" aria-hidden="true" />
                发送中...
              </>
            ) : (
              <>
                <Send size={14} aria-hidden="true" />
                发送到当前会话
              </>
            )}
          </button>
          <button
            type="button"
            className="btn-secondary btn-sm"
            disabled={state.mockInjectPending}
            onClick={() => dispatch({
              type: "set_mock_inject_input",
              value: "这是设备按钮模拟输入，请继续当前任务并给出下一步。",
            })}
          >
            填充示例
          </button>
        </div>
      </div>

      {state.mockInjectMessage && (
        <div
          className={`message-banner voice-panel__message ${
            state.mockInjectOk === true
              ? "message-banner--success"
              : state.mockInjectOk === false
                ? "message-banner--error"
                : "message-banner--muted"
          }`}
        >
          {state.mockInjectMessage}
        </div>
      )}

      {state.mockInjectReply && (
        <div className="message-banner message-banner--muted voice-panel__flow">
          {`模型回复预览：\n${state.mockInjectReply}`}
        </div>
      )}

      {state.deviceVoiceFlow?.phase !== "idle" && (
        <div
          className={`message-banner voice-panel__message ${
            state.deviceVoiceFlow.phase === "done"
              ? "message-banner--success"
              : state.deviceVoiceFlow.phase === "error"
                ? "message-banner--error"
                : "message-banner--muted"
          }`}
        >
          {`设备语音状态：${
            state.deviceVoiceFlow.phase === "done"
              ? "已发送"
              : state.deviceVoiceFlow.phase === "error"
                ? "发送失败"
                : state.deviceVoiceFlow.phase === "waiting_reply"
                  ? "等待回复"
                  : "发送中"
          }${
            state.deviceVoiceFlow.updatedAt
              ? `（${new Date(state.deviceVoiceFlow.updatedAt).toLocaleTimeString()}）`
              : ""
          }\n${state.deviceVoiceFlow.message || ""}`}
        </div>
      )}

      {state.deviceVoiceFlow?.text && (
        <div className="message-banner message-banner--muted voice-panel__flow">
          {`设备识别文本：\n${state.deviceVoiceFlow.text}`}
        </div>
      )}

      {state.deviceVoiceFlow?.reply && (
        <div className="message-banner message-banner--muted voice-panel__flow">
          {`设备语音回复预览：\n${state.deviceVoiceFlow.reply}`}
        </div>
      )}

      {state.audioBridgeMessage && (
        <div
          className={`message-banner voice-panel__message ${
            state.audioBridgeLastResult === "ok"
              ? "message-banner--success"
              : state.audioBridgeLastResult === "error"
                ? "message-banner--error"
                : "message-banner--muted"
          }`}
        >
          {state.audioBridgeMessage}
        </div>
      )}
    </div>
  );
}
