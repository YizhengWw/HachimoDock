/**
 * [Input] Tauri USB audio, transcript, and Agent-delivery events for device PTT utterances.
 * [Output] One monotonic, utterance-scoped draft-then-confirm voice flow with current-to-exact guarded route resolution plus audio relay activity callbacks.
 * [Pos] Dashboard device-voice routing state machine and event-listener boundary.
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md` and `pc/src/.folder.md`.
 */

import { useEffect, useReducer } from "react";
import { listen } from "@tauri-apps/api/event";

const MAX_RETIRED_UTTERANCES = 8;
const TERMINAL_PHASES = new Set(["done", "error", "cancelled"]);
const PHASE_RANK = Object.freeze({
  idle: 0,
  listening: 1,
  partial: 2,
  recognizing: 3,
  finalizing: 3,
  draft_ready: 4,
  injecting: 4,
  submitting: 4,
  waiting_reply: 5,
  submitted: 6,
  done: 7,
  error: 7,
  cancelled: 7,
});

export const DEVICE_VOICE_FLOW_INITIAL_STATE = Object.freeze({
  phase: "idle",
  utteranceId: "",
  revision: 0,
  text: "",
  message: "",
  reply: "",
  ok: null,
  agentId: "",
  sessionId: "",
  isFinal: false,
  composerMode: "",
  composerError: "",
  updatedAt: 0,
});

export const DEVICE_VOICE_ROUTER_INITIAL_STATE = Object.freeze({
  flow: DEVICE_VOICE_FLOW_INITIAL_STATE,
  activeUtteranceId: "",
  retiredUtteranceIds: Object.freeze([]),
  deliveryHandled: false,
});

function normalizeText(value) {
  return typeof value === "string" ? value.trim() : "";
}

function phaseRank(phase) {
  return PHASE_RANK[phase] ?? PHASE_RANK.idle;
}

function monotonicPhase(currentPhase, incomingPhase) {
  const current = normalizeText(currentPhase).toLowerCase() || "idle";
  const incoming = normalizeText(incomingPhase).toLowerCase() || current;
  if (TERMINAL_PHASES.has(current) && !TERMINAL_PHASES.has(incoming)) return current;
  return phaseRank(incoming) < phaseRank(current) ? current : incoming;
}

function nextRouteValue(currentValue, incomingValue, allowAutoResolution = false) {
  const current = normalizeText(currentValue);
  const incoming = normalizeText(incomingValue);
  if (!current) return incoming;
  if (
    allowAutoResolution
    && ["auto", "current"].includes(current)
    && incoming
    && !["auto", "current"].includes(incoming)
  ) {
    return incoming;
  }
  return current;
}

function routeForFlow(flow, action) {
  return {
    agentId: nextRouteValue(flow.agentId, action.agentId),
    sessionId: nextRouteValue(flow.sessionId, action.sessionId, true),
  };
}

function startUtterance(state, utteranceId) {
  const retiredUtteranceIds = state.activeUtteranceId
    ? [
      state.activeUtteranceId,
      ...state.retiredUtteranceIds.filter((id) => id !== state.activeUtteranceId),
    ].slice(0, MAX_RETIRED_UTTERANCES)
    : state.retiredUtteranceIds;
  return {
    ...state,
    flow: DEVICE_VOICE_FLOW_INITIAL_STATE,
    activeUtteranceId: utteranceId,
    retiredUtteranceIds,
    deliveryHandled: false,
  };
}

function selectUtteranceState(state, action) {
  const utteranceId = normalizeText(action.utteranceId);
  if (!utteranceId || utteranceId === state.activeUtteranceId) {
    return { state, utteranceId: utteranceId || state.activeUtteranceId, accepted: true };
  }
  if (state.retiredUtteranceIds.includes(utteranceId)) {
    return { state, utteranceId, accepted: false };
  }
  const incomingPhase = normalizeText(action.phase).toLowerCase();
  if (state.activeUtteranceId && incomingPhase !== "listening") {
    return { state, utteranceId, accepted: false };
  }
  return { state: startUtterance(state, utteranceId), utteranceId, accepted: true };
}

export function deviceVoiceRouterReducer(state, action) {
  switch (action.type) {
    case "transcript": {
      const selected = selectUtteranceState(state, action);
      if (!selected.accepted) return state;
      const baseState = selected.state;
      const flow = baseState.flow;
      const revision = Number(action.revision || 0);
      if (revision < Number(flow.revision || 0)) return state;
      const route = routeForFlow(flow, action);
      return {
        ...baseState,
        activeUtteranceId: selected.utteranceId,
        flow: {
          ...flow,
          phase: monotonicPhase(flow.phase, action.phase || "listening"),
          utteranceId: selected.utteranceId,
          revision,
          text: action.text ?? flow.text,
          message: action.message || flow.message || "",
          reply: "",
          ok: action.ok,
          agentId: route.agentId,
          sessionId: route.sessionId,
          isFinal: flow.isFinal || action.isFinal === true,
          composerMode: action.composerMode || flow.composerMode || "",
          composerError: action.composerError || flow.composerError || "",
          updatedAt: Number(action.nowMs || Date.now()),
        },
      };
    }
    case "progress": {
      const selected = selectUtteranceState(state, action);
      if (!selected.accepted) return state;
      const baseState = selected.state;
      const route = routeForFlow(baseState.flow, action);
      return {
        ...baseState,
        activeUtteranceId: selected.utteranceId,
        flow: {
          ...baseState.flow,
          phase: monotonicPhase(baseState.flow.phase, action.phase || "injecting"),
          utteranceId: selected.utteranceId,
          text: action.text || baseState.flow.text || "",
          message: action.message || "设备语音识别完成，正在发送到当前会话...",
          reply: "",
          ok: null,
          agentId: route.agentId,
          sessionId: route.sessionId,
          updatedAt: Number(action.nowMs || Date.now()),
        },
      };
    }
    case "delivery": {
      const selected = selectUtteranceState(state, action);
      if (!selected.accepted) return state;
      const baseState = selected.state;
      const terminal = action.pending !== true && action.transient !== true;
      if (terminal && baseState.deliveryHandled) return state;
      const phase = action.pending
        ? "waiting_reply"
        : action.transient
          ? "injecting"
          : action.ok
            ? "done"
            : "error";
      const route = routeForFlow(baseState.flow, action);
      return {
        ...baseState,
        activeUtteranceId: selected.utteranceId,
        deliveryHandled: terminal,
        flow: {
          ...baseState.flow,
          phase: monotonicPhase(baseState.flow.phase, phase),
          utteranceId: selected.utteranceId,
          text: action.text || baseState.flow.text || "",
          message: action.message || (action.ok ? "已发送到当前会话" : "发送失败"),
          reply: terminal ? action.reply || "" : "",
          ok: terminal ? action.ok === true : null,
          agentId: route.agentId,
          sessionId: route.sessionId,
          composerMode: action.composerMode || baseState.flow.composerMode || "",
          composerError: ["visible", "focused-input"].includes(action.composerMode)
            ? ""
            : action.composerError || baseState.flow.composerError || "",
          updatedAt: Number(action.nowMs || Date.now()),
        },
      };
    }
    default:
      return state;
  }
}

function transcriptMessage(payload, phase, composerMode) {
  const visibleAgentLabel = normalizeText(payload?.agentId).toLowerCase() === "claude-code"
    ? "Claude"
    : "ChatGPT（Codex）";
  if (phase === "listening") {
    if (composerMode === "visible") {
      return `正在聆听，识别文字会实时同步到 ${visibleAgentLabel} 输入框。`;
    }
    if (composerMode === "focused-input") {
      return "正在聆听；松开后只把文字写入 MiMoCode 当前光标。";
    }
    return "正在聆听并实时识别。";
  }
  if (phase === "partial") {
    if (composerMode === "visible") {
      return `正在实时识别并同步到 ${visibleAgentLabel}；松开后保留为草稿，不自动发送。`;
    }
    if (composerMode === "focused-input") {
      return "正在实时识别；松开后保留为草稿，不自动发送。";
    }
    return "正在实时识别；松开后保留为草稿，不自动发送。";
  }
  if (phase === "finalizing" || phase === "recognizing") {
    return "正在把最终识别文字写入输入框...";
  }
  if (phase === "draft_ready") {
    if (composerMode === "focused-input") {
      return "语音文字已写入 MiMoCode；请短按确认键发送（默认 SW3）。";
    }
    if (composerMode === "agent-bus") {
      return "语音文字已识别为待发送草稿；请短按确认键发送（默认 SW3）。";
    }
    return `语音文字已写入 ${visibleAgentLabel} 输入框；请短按确认键发送（默认 SW3）。`;
  }
  if (phase === "submitting") {
    if (composerMode === "visible") return `已收到确认键，正在通过 ${visibleAgentLabel} 输入框发送...`;
    if (composerMode === "focused-input") return "已收到确认键，正在发送 MiMoCode 草稿...";
    return "已收到确认键，正在发送到当前会话...";
  }
  if (phase === "submitted") {
    return composerMode === "focused-input"
      ? "已通过设备确认键发送 MiMoCode 草稿。"
      : `已通过 ${visibleAgentLabel} 可见输入框发送到当前会话。`;
  }
  if (phase === "cancelled") {
    return normalizeText(payload.error) || "录音目标已变化，本次语音已取消。";
  }
  if (phase === "error") {
    return normalizeText(payload.error) || "设备语音处理失败。";
  }
  return "";
}

function audioActivity(payload) {
  const phase = normalizeText(payload.phase).toLowerCase();
  if (!phase || phase === "status") return null;
  const ok = payload.ok !== false;
  const bytes = Number(payload.bytes || 0);
  const durationMs = Number(
    payload.forwardedDurationMs || payload.durationMs || (bytes > 0 ? bytes / 32 : 0),
  );
  let message = "";
  if (phase === "cancelled") {
    message = normalizeText(payload.error) || "设备录音已取消。";
  } else if (!ok) {
    message = `板端录音处理失败：${normalizeText(payload.error) || "音频流校验失败"}`;
  } else if (phase === "begin") {
    message = "设备麦克风录音中，音频正通过 USB 转发...";
  } else if (phase === "streaming") {
    message = `设备麦克风录音中，已转发 ${(durationMs / 1000).toFixed(1)} 秒音频...`;
  } else if (phase === "end") {
    message = `设备录音完成，共 ${(durationMs / 1000).toFixed(1)} 秒。`;
  } else if (phase === "recognizing") {
    message = "正在识别设备麦克风录音...";
  } else if (phase === "recognized") {
    message = "识别完成，正在写入输入框草稿...";
  }
  return message ? { phase, ok, message } : null;
}

export function useDeviceVoiceRouter({ onAudioActivity } = {}) {
  const [routerState, dispatch] = useReducer(
    deviceVoiceRouterReducer,
    DEVICE_VOICE_ROUTER_INITIAL_STATE,
  );

  useEffect(() => {
    let disposed = false;
    let unlistenUsbMessage = null;
    let unlistenUsbResult = null;
    let unlistenUsbAudio = null;
    let unlistenVoiceTranscript = null;

    const setupListeners = async () => {
      unlistenUsbMessage = await listen("usb-message", (event) => {
        const envelope = event?.payload && typeof event.payload === "object" ? event.payload : {};
        if (envelope.topic !== "input/action") return;
        const payload = envelope.payload && typeof envelope.payload === "object"
          ? envelope.payload
          : {};
        if (normalizeText(payload.view).toLowerCase() !== "voice_input") return;
        const text = normalizeText(payload.state);
        if (!text) return;
        dispatch({
          type: "progress",
          phase: "injecting",
          utteranceId: normalizeText(payload.voiceUtteranceId || payload.utteranceId),
          text,
          message: "已收到确认键，正在发送语音草稿...",
        });
      });

      unlistenUsbResult = await listen("usb-input-action-result", (event) => {
        const payload = event?.payload && typeof event.payload === "object" ? event.payload : {};
        const view = normalizeText(payload.view).toLowerCase();
        if (view && view !== "voice_input") return;
        const ok = payload.ok === true;
        const transient = payload.transient === true;
        const pending = payload.pending === true;
        const agentId = normalizeText(payload.agentId);
        const sessionId = normalizeText(payload.sessionId);
        const composerMode = normalizeText(payload.composerMode).toLowerCase();
        const reply = normalizeText(
          payload.tokenPreview
          || payload.replyPreview
          || payload.response?.tokenPreview,
        );
        const defaultSuccessMessage = `已发送到 ${agentId || "当前工具"} · 会话 ${sessionId || "auto"}`;
        const baseMessage = normalizeText(payload.message)
          || (!ok ? normalizeText(payload.error) : "")
          || (ok ? defaultSuccessMessage : "发送失败");
        dispatch({
          type: "delivery",
          utteranceId: normalizeText(payload.utteranceId || payload.voiceUtteranceId),
          ok,
          transient,
          pending,
          text: normalizeText(payload.text),
          message: !ok && transient
            ? `桥接连接瞬时抖动，正在确认是否已送达...\n${baseMessage}`
            : baseMessage,
          reply,
          agentId,
          sessionId,
          composerMode,
          composerError: normalizeText(payload.composerError),
        });
      });

      unlistenUsbAudio = await listen("usb-audio-stream", (event) => {
        const payload = event?.payload && typeof event.payload === "object" ? event.payload : {};
        const activity = audioActivity(payload);
        if (activity) onAudioActivity?.(activity);
      });

      unlistenVoiceTranscript = await listen("voice-transcript", (event) => {
        const payload = event?.payload && typeof event.payload === "object" ? event.payload : {};
        const phase = normalizeText(payload.phase).toLowerCase() || "listening";
        const composerMode = normalizeText(payload.composerMode).toLowerCase();
        dispatch({
          type: "transcript",
          phase,
          utteranceId: normalizeText(payload.utteranceId),
          revision: Number(payload.revision || 0),
          text: normalizeText(payload.text),
          message: transcriptMessage(payload, phase, composerMode),
          ok: payload.ok !== false,
          agentId: normalizeText(payload.agentId),
          sessionId: normalizeText(payload.sessionId),
          isFinal: payload.isFinal === true,
          composerMode,
          composerError: normalizeText(payload.composerError),
        });
      });

      if (disposed) {
        unlistenUsbMessage?.();
        unlistenUsbResult?.();
        unlistenUsbAudio?.();
        unlistenVoiceTranscript?.();
      }
    };

    setupListeners().catch((error) => {
      console.warn("[voice] failed to listen for USB voice action events", error);
    });
    return () => {
      disposed = true;
      unlistenUsbMessage?.();
      unlistenUsbResult?.();
      unlistenUsbAudio?.();
      unlistenVoiceTranscript?.();
    };
  }, [onAudioActivity]);

  return {
    deviceVoiceFlow: routerState.flow,
    activeUtteranceId: routerState.activeUtteranceId,
  };
}
