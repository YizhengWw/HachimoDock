/**
 * [Input] `useDeviceContext()` realtime chat status, the current on-device appearance (with its
 *         `personaVoice`), USB connectivity, and the start/stop helpers.
 * [Output] The 实时对话 card on the device dashboard: which persona will talk, whether the board
 *          key is bound, live state (listening / thinking / speaking), the last transcript and
 *          reply, errors, prerequisite checks and actionable setup links, plus PC-side 开始/结束聊天 buttons.
 * [Pos] dashboard node in ref/src/dashboard, rendered by DeviceDashboard.jsx.
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { AudioLines, Loader, Mic, Square } from "lucide-react";
import { useDeviceContext } from "../shell/DeviceContext.jsx";
import { personaSummaryLabel } from "../lib/persona-voice.js";
import { realtimeReadiness } from "../lib/usage-help.js";
import { API_CONFIGURATION_UPDATED_EVENT } from "../lib/api-configuration.js";
import UsageHelp from "../UsageHelp.jsx";

export const REALTIME_STATE_LABELS = Object.freeze({
  idle: "未开始",
  preparing: "准备中",
  listening: "在听",
  thinking: "在想",
  speaking: "在说",
  ended: "已结束",
});

export function realtimeStateLabel(state) {
  return REALTIME_STATE_LABELS[state] || String(state || "未开始");
}

export default function RealtimeChatPanel({ keyBound = false, onOpenApiSettings, onOpenGallery, onOpenPersona }) {
  const { realtimeChat, startRealtimeChat, stopRealtimeChat, currentDisplay, deviceOnline } = useDeviceContext();
  const [busy, setBusy] = useState(false);
  const [localError, setLocalError] = useState("");
  const appearance = currentDisplay?.appearance || null;
  const personaReady = Boolean(appearance?.personaVoice?.configured);
  const active = Boolean(realtimeChat?.active);
  const state = active ? realtimeChat.state : "idle";
  const error = localError || realtimeChat?.error || "";
  const [check, setCheck] = useState({ loading: true });
  const [checkRevision, setCheckRevision] = useState(0);
  useEffect(() => {
    let cancelled = false;
    let sequence = 0;
    const load = async () => {
      const request = ++sequence;
      setCheck(current => ({ ...current, loading: true }));
      try {
        const [settings, speech] = await Promise.all([invoke("load_voice_chat_settings"), invoke("load_device_asr_settings")]);
        if (!cancelled && request === sequence) setCheck({ loading: false, settings: { ...settings, asrConfigured: speech?.configured === true } });
      } catch {
        if (!cancelled && request === sequence) setCheck({ loading: false, failed: true });
      }
    };
    load();
    window.addEventListener(API_CONFIGURATION_UPDATED_EVENT, load);
    window.addEventListener("focus", load);
    return () => { cancelled = true; window.removeEventListener(API_CONFIGURATION_UPDATED_EVENT, load); window.removeEventListener("focus", load); };
  }, [checkRevision]);
  const readiness = realtimeReadiness({ deviceOnline, appearance, personaReady, ...check });
  const nextAction = readiness?.action === "api" ? onOpenApiSettings
    : readiness?.action === "persona" ? onOpenPersona
    : readiness?.action === "appearance" ? onOpenGallery
    : readiness?.action === "retry" ? () => setCheckRevision(value => value + 1) : null;

  const handleStart = useCallback(async () => {
    setBusy(true);
    setLocalError("");
    try {
      await startRealtimeChat();
    } catch (err) {
      setLocalError(err?.message || String(err));
    } finally {
      setBusy(false);
    }
  }, [startRealtimeChat]);

  const handleStop = useCallback(async () => {
    setBusy(true);
    try {
      await stopRealtimeChat("ui");
    } catch (err) {
      setLocalError(err?.message || String(err));
    } finally {
      setBusy(false);
    }
  }, [stopRealtimeChat]);

  return (
    <div className="realtime-chat-panel">
      <div className="realtime-chat-panel__row">
        <div className="realtime-chat-panel__persona">
          <Mic size={14} />
          <strong>{appearance ? appearance.name : "设备上还没有形象"}</strong>
          <span className={"muted small" + (personaReady ? "" : " realtime-chat-panel__warn")}>
            {appearance ? personaSummaryLabel(appearance.personaVoice) : "先在画廊里应用一个形象"}
          </span>
        </div>
        <span className={`realtime-chat-panel__state is-${state}`}>
          {active && state !== "ended" ? <AudioLines size={13} /> : null}
          {realtimeStateLabel(state)}
        </span>
      </div>
      <UsageHelp onOpenApiSettings={onOpenApiSettings} />
      {!keyBound && <p className="muted small">未绑定设备快捷键时，也可以点击下方「开始聊天」。</p>}
      {!active && readiness && <div className="usage-help" role="status">
        <p>{readiness.message}</p>
        {nextAction && <button type="button" className="btn-secondary btn-sm" onClick={nextAction}>
          {readiness.action === "persona" ? "设置人设与声音" : readiness.action === "appearance" ? "去形象画廊" : readiness.action === "retry" ? "重新检查" : "去配置"}
        </button>}
      </div>}
      {appearance?.personaVoice?.voiceMigrationNotice && <p className="muted small">{appearance.personaVoice.voiceMigrationNotice}</p>}
      {(realtimeChat?.transcript || realtimeChat?.reply) && (
        <div className="realtime-chat-panel__dialog">
          {realtimeChat.transcript && <div><span className="muted small">你：</span>{realtimeChat.transcript}</div>}
          {realtimeChat.reply && <div><span className="muted small">{realtimeChat.displayName || "它"}：</span>{realtimeChat.reply}</div>}
        </div>
      )}
      {error && <div role="alert" className="message-banner message-banner--error">实时对话失败：{error}</div>}
      <details className="usage-help"><summary>问题排查与日志</summary><p>遇到问题时，可提供报错信息及诊断日志。日志位于应用数据目录 / logs / realtime-chat.jsonl，不记录录音与完整对话。</p></details>
      {!active && realtimeChat?.reason && realtimeChat.reason !== "ui" && (
        <div className="muted small">上次结束：{realtimeChat.reason === "idle" ? "60 秒无人说话" : realtimeChat.reason === "key" ? "按键结束" : realtimeChat.reason}</div>
      )}
      <div className="realtime-chat-panel__actions">
        {active ? (
          <button type="button" className="btn-secondary btn-sm" onClick={handleStop} disabled={busy}>
            {busy ? <Loader size={14} className="spin" /> : <Square size={14} />} 结束聊天
          </button>
        ) : (
          <button
            type="button"
            className="btn-primary btn-sm"
            onClick={handleStart}
            disabled={busy || Boolean(readiness)}
            title={readiness?.message || "和设备上的宠物开始聊天"}
          >
            {busy ? <Loader size={14} className="spin" /> : <Mic size={14} />} 开始聊天
          </button>
        )}
      </div>
    </div>
  );
}
