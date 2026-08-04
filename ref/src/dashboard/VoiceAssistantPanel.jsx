/**
 * [Input] state (busStatus/busSessions/busSessionId/voiceRuntime/audioBridge{*}/mockInject/deviceVoiceFlow/selectedAgentId/deviceOnline) + dispatch + toggleAudioBridge/sendMockButtonInject + voiceConfig + selectedTrigger + onVoiceConfigChange/onVoiceEnabledChange + API-settings navigation.
 * [Output] Region 4: a compact voice console with immediate saved-ASR runtime rearming, ChatGPT（Codex）/Claude-visible and MiMoCode-caret delivery labels, non-prompting macOS trust checks, native system-consent retry, and foreground recovery tips.
 * [Pos] component node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import React, { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  Activity,
  CheckCircle2,
  ChevronDown,
  Cloud,
  ExternalLink,
  FlaskConical,
  Loader,
  Mic,
  MicOff,
  RefreshCw,
  Send,
  ShieldAlert,
} from "lucide-react";
import { API_CONFIGURATION_UPDATED_EVENT } from "../lib/api-configuration.js";
import Switch from "../shell/Switch";

export function buildVoiceSummary(voiceConfig, selectedTrigger) {
  if (!voiceConfig?.enabled) return "未开启";
  const trigger = selectedTrigger?.label || "默认触发";
  return `已开启 · ${trigger}`;
}

export function formatVoiceUserMessage(value) {
  const message = typeof value === "string"
    ? value.replace(/\bError:\s*/gi, "").trim()
    : "";
  if (!message) return "";
  if (/设备麦克风.*信令.*USB.*送达|未通过 USB 送达/i.test(message)) {
    return "设备未收到语音监听指令，请确认 USB 连接后重试。";
  }
  return message.replace(
    /设备麦克风自动恢复失败[:：]\s*/,
    "语音监听恢复失败：",
  );
}

export const VISIBLE_COMPOSER_DEV_PATHS = Object.freeze({
  macos: "ref/src-tauri/target/debug/pet-manager-tauri",
  windows: String.raw`ref\src-tauri\target\debug\pet-manager-tauri.exe`,
});

export function detectDesktopPlatform(navigatorLike) {
  const target = navigatorLike
    || (typeof navigator === "undefined" ? null : navigator);
  const descriptor = [
    target?.userAgentData?.platform,
    target?.platform,
    target?.userAgent,
  ].filter(Boolean).join(" ").toLowerCase();
  if (descriptor.includes("win")) return "windows";
  if (descriptor.includes("mac")) return "macos";
  return "other";
}

export function needsVisibleComposerGuidance(state) {
  const flow = state?.deviceVoiceFlow || {};
  const hasDeliveryError = state?.audioBridgeLastResult === "error"
    || flow.phase === "error"
    || flow.ok === false;
  if (!hasDeliveryError) return false;

  const details = [
    state?.audioBridgeMessage,
    flow.message,
    flow.composerError,
  ].filter((value) => typeof value === "string").join("\n");
  const isVisibleDesktopAgent = ["codex", "claude-code"].includes(state?.selectedAgentId)
    || ["codex", "claude-code"].includes(flow.agentId)
    || /ChatGPT|Codex|Claude/i.test(details);
  const isMimocode = state?.selectedAgentId === "mimocode"
    || flow.agentId === "mimocode"
    || /\bMiMoCode\b/i.test(details);
  return (isVisibleDesktopAgent || isMimocode) && (
    /(macOS|Windows).*辅助功能/i.test(details)
    || /(macOS|Windows).*输入焦点无法确认/i.test(details)
    || /(macOS|Windows).*输入框已有用户草稿/i.test(details)
    || /(ChatGPT（Codex）|Codex|Claude) 前台会话未定位/i.test(details)
    || /(ChatGPT（Codex）|Codex|Claude) 前台提交失败/i.test(details)
  );
}

export const needsMacosAccessibilityGuidance = needsVisibleComposerGuidance;

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

function voicePhaseLabel(phase) {
  return ({
    listening: "正在聆听",
    partial: "实时识别中",
    recognizing: "确认文本中",
    finalizing: "确认文本中",
    submitting: "发送中",
    injecting: "发送中",
    waiting_reply: "等待回复",
    submitted: "已发送",
    done: "已发送",
    cancelled: "已取消",
    error: "发送失败",
  })[phase] || "处理中";
}

export default function VoiceAssistantPanel({
  state,
  dispatch,
  toggleAudioBridge,
  sendMockButtonInject,
  voiceConfig,
  selectedTrigger,
  onVoiceConfigChange,
  onVoiceEnabledChange,
  onCredentialReady,
  onOpenApiSettings,
}) {
  const [asrState, setAsrState] = useState({
    loading: true,
    configured: false,
    tone: "muted",
    message: "正在读取云端识别配置...",
  });
  const [accessibilityAction, setAccessibilityAction] = useState({
    pending: "",
    tone: "",
    message: "",
  });
  const [accessibilityPermission, setAccessibilityPermission] = useState({
    loading: false,
    trusted: null,
    error: "",
  });

  useEffect(() => {
    let cancelled = false;
    const loadAsrState = ({ resume = false } = {}) => {
      invoke("load_device_asr_settings")
        .then((status) => {
          if (cancelled) return;
          dispatch({
            type: "set_voice_runtime",
            value: {
              ...status,
              running: status?.configured === true,
            },
          });
          setAsrState({
            loading: false,
            configured: status?.configured === true,
            tone: status?.configured ? "success" : "muted",
            message: status?.message || "请前往 API 配置补充语音识别凭据",
          });
          if (resume && status?.configured === true) {
            onCredentialReady?.();
          }
        })
        .catch((error) => {
          if (cancelled) return;
          setAsrState({
            loading: false,
            configured: false,
            tone: "error",
            message: `读取云端识别配置失败: ${error}`,
          });
        });
    };
    loadAsrState();
    const handleApiConfigurationUpdated = (event) => {
      if (event?.detail?.providerId && event.detail.providerId !== "volcengine-asr") return;
      loadAsrState({ resume: true });
    };
    window.addEventListener(API_CONFIGURATION_UPDATED_EVENT, handleApiConfigurationUpdated);
    return () => {
      cancelled = true;
      window.removeEventListener(API_CONFIGURATION_UPDATED_EVENT, handleApiConfigurationUpdated);
    };
  }, [dispatch, onCredentialReady]);

  const agents = Array.isArray(state.busStatus?.agents) ? state.busStatus.agents : [];
  const selectedAgent = agents.find((agent) => agent.agentId === state.selectedAgentId) || null;
  const ready = selectedAgent?.ready === true;
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
  const triggerLabel = selectedTrigger?.label || "默认触发";
  const voicePhase = state.deviceVoiceFlow?.phase || "idle";
  const desktopPlatform = detectDesktopPlatform();
  const showAccessibilityGuidance = needsVisibleComposerGuidance(state);
  const isMimocodeVoice = state.selectedAgentId === "mimocode"
    || state.deviceVoiceFlow?.agentId === "mimocode";
  const visibleVoiceAgentId = state.deviceVoiceFlow?.agentId || state.selectedAgentId;
  const visibleVoiceAgentLabel = visibleVoiceAgentId === "claude-code"
    ? "Claude"
    : "ChatGPT（Codex）";
  const hasLiveActivity = voicePhase !== "idle"
    || Boolean(state.deviceVoiceFlow?.text)
    || Boolean(state.deviceVoiceFlow?.reply)
    || showAccessibilityGuidance;
  const formattedAudioBridgeMessage = formatVoiceUserMessage(state.audioBridgeMessage);
  const listenHint = state.audioBridgeEnabled
    ? "设备麦克风监听中，等待已配置按键触发"
    : state.audioBridgePending
      ? "正在把监听状态同步到设备"
      : !voiceConfig.enabled
        ? "启用按键语音后，才可以启动设备监听"
        : audioBlockingReason
          ? "语音通道暂未就绪，请检查 Agent 与识别服务"
          : boardOffline
            ? "设备当前离线，重新连接后可启动监听"
            : "通过 USB 接收设备麦克风音频，不会使用电脑麦克风";

  useEffect(() => {
    if (!showAccessibilityGuidance || desktopPlatform !== "macos") {
      return undefined;
    }
    let cancelled = false;
    setAccessibilityPermission({
      loading: true,
      trusted: null,
      error: "",
    });
    invoke("check_codex_accessibility_permission")
      .then((result) => {
        if (cancelled) return;
        setAccessibilityPermission({
          loading: false,
          trusted: result?.trusted === true,
          error: "",
        });
      })
      .catch((error) => {
        if (cancelled) return;
        setAccessibilityPermission({
          loading: false,
          trusted: null,
          error: formatVoiceUserMessage(String(error)),
        });
      });
    return () => {
      cancelled = true;
    };
  }, [
    desktopPlatform,
    showAccessibilityGuidance,
    state.deviceVoiceFlow?.updatedAt,
  ]);

  const requestAccessibilityPermission = async () => {
    setAccessibilityAction({
      pending: "request",
      tone: "",
      message: "",
    });
    try {
      const result = await invoke("request_codex_accessibility_permission");
      const trusted = result?.trusted === true;
      setAccessibilityPermission({
        loading: false,
        trusted,
        error: "",
      });
      setAccessibilityAction({
        pending: "",
        tone: trusted ? "success" : "muted",
        message: trusted
          ? "辅助功能权限已确认，无需再次授权。"
          : "已请求系统授权。请在 macOS 弹窗中点击“打开系统设置”，打开 Pet Manager 开关后返回。",
      });
    } catch (error) {
      setAccessibilityAction({
        pending: "",
        tone: "error",
        message: `无法请求系统授权：${formatVoiceUserMessage(String(error))}`,
      });
    }
  };

  const recheckAccessibilityPermission = async () => {
    setAccessibilityAction({
      pending: "check",
      tone: "",
      message: "",
    });
    try {
      const result = await invoke("check_codex_accessibility_permission");
      const trusted = result?.trusted === true;
      setAccessibilityPermission({
        loading: false,
        trusted,
        error: "",
      });
      setAccessibilityAction({
        pending: "",
        tone: trusted ? "success" : "error",
        message: trusted
          ? isMimocodeVoice
            ? "辅助功能权限已确认。请保持 MiMoCode 终端在前台，并把光标停在可输入位置。"
            : `辅助功能权限已确认，不需要再次添加或输入密码。请继续检查目标 ${visibleVoiceAgentLabel} 会话和空输入框。`
          : "仍未获得权限。请在辅助功能列表中打开 Pet Manager；若已打开，请关闭后重新开启。",
      });
    } catch (error) {
      setAccessibilityAction({
        pending: "",
        tone: "error",
        message: `权限检查失败：${formatVoiceUserMessage(String(error))}`,
      });
    }
  };

  return (
    <div className="voice-panel voice-panel--compact">
      <section
        className={`voice-panel__command${voiceConfig.enabled ? " is-on" : ""}`}
        aria-label="按键语音控制"
      >
        <span className="voice-panel__command-icon" aria-hidden="true">
          {voiceConfig.enabled ? <Mic size={19} /> : <MicOff size={19} />}
        </span>
        <div className="voice-panel__command-copy">
          <div className="voice-panel__command-title">
            <strong>{voiceConfig.enabled ? "按键语音已启用" : "按键语音未启用"}</strong>
            <span className={`voice-panel__status-chip${asrState.configured ? " is-success" : ""}`}>
              {asrState.configured ? "ASR 已就绪" : "ASR 待配置"}
            </span>
            <span className="voice-panel__status-chip">
              {triggerLabel}
            </span>
          </div>
          <span>{listenHint}</span>
        </div>
        <Switch
          className="voice-panel__command-switch"
          checked={voiceConfig.enabled}
          onCheckedChange={(enabled) => (
            onVoiceEnabledChange
              ? onVoiceEnabledChange(enabled)
              : onVoiceConfigChange({ enabled })
          )}
          label="启用"
          ariaLabel="启用按键语音"
          title="控制设备端按键语音功能是否启用"
        />
        <button
          type="button"
          className="btn-primary btn-sm voice-panel__listen-btn"
          onClick={() => toggleAudioBridge(state.audioBridgeEnabled ? "stop" : "start")}
          disabled={
            state.audioBridgePending
            || (!state.audioBridgeEnabled && (!!audioBlockingReason || !voiceConfig.enabled))
          }
          title={
            state.audioBridgeEnabled
              ? "停止设备麦克风监听"
              : !voiceConfig.enabled
                ? "请先启用按键语音"
                : audioBlockingReason || "使用设备麦克风录音并通过 USB 转发。"
          }
        >
          {state.audioBridgePending ? (
            <Loader size={14} className="spin" aria-hidden="true" />
          ) : state.audioBridgeEnabled ? (
            <MicOff size={14} aria-hidden="true" />
          ) : (
            <Mic size={14} aria-hidden="true" />
          )}
          {state.audioBridgePending
            ? "同步中…"
            : state.audioBridgeEnabled
              ? "停止语音监听"
              : "启动语音监听"}
        </button>
        {formattedAudioBridgeMessage && (
          <div
            className={`voice-panel__command-message ${
              state.audioBridgeLastResult === "ok"
                ? "is-success"
                : state.audioBridgeLastResult === "error"
                  ? "is-error"
                  : ""
            }`}
            role="status"
          >
            {formattedAudioBridgeMessage}
          </div>
        )}
      </section>

      <div className="voice-panel__advanced-grid">
        <button
          type="button"
          className="voice-panel__advanced voice-panel__api-settings"
          onClick={onOpenApiSettings}
        >
          <span className="voice-panel__advanced-summary">
            <span className="voice-panel__advanced-icon" aria-hidden="true">
              <Cloud size={17} />
            </span>
            <span className="voice-panel__advanced-copy">
              <strong>语音识别 API</strong>
              <small>{asrState.message}</small>
            </span>
            <span className={`voice-panel__asr-state is-${asrState.tone}`}>
              {asrState.loading ? (
                <Loader size={14} className="spin" aria-hidden="true" />
              ) : (
                <CheckCircle2 size={14} aria-hidden="true" />
              )}
              {asrState.loading ? "读取中" : asrState.configured ? "已配置" : "待配置"}
            </span>
            <span className="voice-panel__api-cta">前往 API 配置</span>
          </span>
        </button>

        <details className="voice-panel__advanced voice-panel__advanced--diagnostics">
          <summary className="voice-panel__advanced-summary">
            <span className="voice-panel__advanced-icon" aria-hidden="true">
              <FlaskConical size={17} />
            </span>
            <span className="voice-panel__advanced-copy">
              <strong>诊断与测试</strong>
              <small>绕过麦克风与 ASR，直接验证当前 Agent 会话</small>
            </span>
            <span className="voice-panel__advanced-tag">高级</span>
            <ChevronDown
              className="voice-panel__advanced-chevron"
              size={16}
              aria-hidden="true"
            />
          </summary>
          <div className="voice-panel__advanced-body voice-panel__diagnostics-body">
          <div className="voice-panel__mock voice-panel__compose">
            <label className="ui-field voice-panel__compose-field" htmlFor="voice-mock-inject-input">
              <span className="ui-field__label">模拟文本注入</span>
              <textarea
                id="voice-mock-inject-input"
                className="ui-control ui-control--textarea voice-panel__mock-textarea"
                value={state.mockInjectInput || ""}
                onChange={(event) => dispatch({
                  type: "set_mock_inject_input",
                  value: event.target.value,
                })}
                placeholder="输入要发送到当前会话的测试文本"
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
              {formatVoiceUserMessage(state.mockInjectMessage)}
            </div>
          )}

          {state.mockInjectReply && (
            <div className="message-banner message-banner--muted voice-panel__flow">
              {`模型回复预览：\n${state.mockInjectReply}`}
            </div>
          )}
          </div>
        </details>
      </div>

      {hasLiveActivity && (
        <section className="voice-panel__activity" aria-label="本次语音活动">
          <div className="voice-panel__activity-head">
            <Activity size={16} aria-hidden="true" />
            <strong>本次语音</strong>
          </div>
          {voicePhase !== "idle" && (
            <div
              className={`message-banner voice-panel__message ${
                voicePhase === "done"
                  ? "message-banner--success"
                  : voicePhase === "error"
                    ? "message-banner--error"
                    : "message-banner--muted"
              }`}
            >
              {`设备语音状态：${
                voicePhaseLabel(voicePhase)
              }${
                state.deviceVoiceFlow.updatedAt
                  ? `（${new Date(state.deviceVoiceFlow.updatedAt).toLocaleTimeString()}）`
                  : ""
              }\n${formatVoiceUserMessage(state.deviceVoiceFlow.message)}`}
            </div>
          )}

          {showAccessibilityGuidance && (
            <aside
              className={`voice-panel__accessibility-guide${
                accessibilityPermission.trusted === true ? " is-trusted" : ""
              }`}
              aria-labelledby="voice-accessibility-guide-title"
            >
              <span className="voice-panel__accessibility-guide-icon" aria-hidden="true">
                <ShieldAlert size={18} />
              </span>
              <div className="voice-panel__accessibility-guide-copy">
                <strong id="voice-accessibility-guide-title">
                  {desktopPlatform === "windows"
                    ? "请检查 Windows 前台输入条件"
                    : accessibilityPermission.loading
                      ? "正在确认辅助功能权限"
                      : accessibilityPermission.trusted === true
                        ? "辅助功能已确认，无需重复授权"
                        : "请确认 Pet Manager 的辅助功能权限"}
                </strong>
                {desktopPlatform === "windows" ? (
                  <p>
                    Windows 不需要把 Pet Manager 添加到“辅助功能”列表。请让 Pet Manager
                    与 {visibleVoiceAgentLabel} 使用相同权限级别，并保持 {visibleVoiceAgentLabel} 窗口可见、输入框为空。
                  </p>
                ) : accessibilityPermission.trusted === true ? (
                  <p>
                    {isMimocodeVoice
                      ? "系统已允许当前 Pet Manager 写入前台终端。此次失败来自当前光标定位，"
                      : `系统已允许当前 Pet Manager 控制 ${visibleVoiceAgentLabel}。此次失败来自目标会话或输入框定位，`}
                    <b>请不要再次添加权限或重复输入系统密码</b>
                    。
                  </p>
                ) : accessibilityPermission.loading ? (
                  <p>正在读取当前进程的真实授权状态，不会弹出系统授权窗口。</p>
                ) : (
                  <p>
                    前往
                    {" "}
                    <b>系统设置 → 隐私与安全性 → 辅助功能</b>
                    ，允许当前 Pet Manager 进程
                    {isMimocodeVoice ? "向前台终端输入文字" : `控制 ${visibleVoiceAgentLabel}`}
                    。
                  </p>
                )}
              </div>
              {desktopPlatform === "macos"
                && !accessibilityPermission.loading
                && accessibilityPermission.trusted !== true && (
                <div className="voice-panel__accessibility-guide-actions">
                  <button
                    type="button"
                    className="btn-secondary btn-sm"
                    onClick={requestAccessibilityPermission}
                    disabled={Boolean(accessibilityAction.pending)}
                  >
                    {accessibilityAction.pending === "request" ? (
                      <Loader size={14} className="spin" aria-hidden="true" />
                    ) : (
                      <ExternalLink size={14} aria-hidden="true" />
                    )}
                    请求系统授权
                  </button>
                  <button
                    type="button"
                    className="btn-ghost btn-sm"
                    onClick={recheckAccessibilityPermission}
                    disabled={Boolean(accessibilityAction.pending)}
                  >
                    <RefreshCw
                      size={14}
                      className={accessibilityAction.pending === "check" ? "spin" : ""}
                      aria-hidden="true"
                    />
                    重新检查
                  </button>
                </div>
              )}
              <div className="voice-panel__accessibility-guide-tip">
                <span>DEV TIP · {desktopPlatform === "windows" ? "WINDOWS" : "MACOS"}</span>
                {desktopPlatform === "windows" ? (
                  <>
                    <p>
                      从项目根目录运行下方文件。不要只提升其中一个程序的管理员权限；
                      推荐 Pet Manager 与 {visibleVoiceAgentLabel} 都以普通用户身份运行。
                    </p>
                    <code>{VISIBLE_COMPOSER_DEV_PATHS.windows}</code>
                  </>
                ) : accessibilityPermission.trusted === true ? (
                  <>
                    {isMimocodeVoice ? (
                      <p>
                        无需再进入系统设置。请保持 MiMoCode 终端在前台，并把光标停在输入位置；
                        松开设备按键后会写入最终文字并自动回车。
                      </p>
                    ) : (
                      <p>
                        无需再进入系统设置。请保持设备选中的 {visibleVoiceAgentLabel} 任务处于可打开状态，
                        并确保输入框没有用户草稿；客户端会继续自动定位和聚焦。
                      </p>
                    )}
                  </>
                ) : (
                  <>
                    <p>
                      在辅助功能页点“＋”，文件选择器按
                      {" "}
                      <kbd>⌘⇧G</kbd>
                      ，输入项目的绝对路径并选择下方 Dev 可执行文件；启用开关后完全重启客户端。
                    </p>
                    <code>{VISIBLE_COMPOSER_DEV_PATHS.macos}</code>
                  </>
                )}
              </div>
              {accessibilityPermission.error && (
                <p className="voice-panel__accessibility-guide-result is-error" role="status">
                  {`读取权限状态失败：${accessibilityPermission.error}`}
                </p>
              )}
              {accessibilityAction.message && (
                <p
                  className={`voice-panel__accessibility-guide-result is-${
                    accessibilityAction.tone || "muted"
                  }`}
                  role="status"
                >
                  {accessibilityAction.message}
                </p>
              )}
            </aside>
          )}

          {state.deviceVoiceFlow?.text && (
            <div className="voice-panel__transcript" aria-live="polite">
              <div className="voice-panel__transcript-head">
                <span>{state.deviceVoiceFlow.isFinal ? "最终识别文本" : "实时识别文本"}</span>
                {state.deviceVoiceFlow.composerMode === "visible" && (
                  <span className="voice-panel__composer-mode is-visible">
                    {visibleVoiceAgentLabel} 可见同步
                  </span>
                )}
                {state.deviceVoiceFlow.composerMode === "focused-input" && (
                  <span className="voice-panel__composer-mode is-visible">
                    MiMoCode 光标提交
                  </span>
                )}
              </div>
              <div className="voice-panel__transcript-text">{state.deviceVoiceFlow.text}</div>
            </div>
          )}

          {state.deviceVoiceFlow?.reply && (
            <div className="message-banner message-banner--muted voice-panel__flow">
              {`设备语音回复预览：\n${state.deviceVoiceFlow.reply}`}
            </div>
          )}
        </section>
      )}
    </div>
  );
}
