/**
 * [Input] Tauri USB audio, transcript, and Agent-delivery events for device PTT utterances.
 * [Output] One monotonic, utterance-scoped draft-then-confirm voice flow with current-to-exact guarded route resolution plus audio relay activity callbacks.
 * [Pos] Dashboard device-voice routing state machine and event-listener boundary.
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md` and `pc/src/.folder.md`.
 */

import { useEffect, useReducer, useRef } from "react";
import { listen } from "@tauri-apps/api/event";

const MAX_RETIRED_UTTERANCES = 8;
const TERMINAL_PHASES = new Set(["done", "error", "submit_unknown", "cancelled"]);
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
  submit_unknown: 7,
  cancelled: 7,
});

export const DEVICE_VOICE_FLOW_INITIAL_STATE = Object.freeze({
  phase: "idle",
  utteranceId: "",
  captureId: "",
  boardDeviceId: "",
  connectionId: "",
  bootId: "",
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
  retiredCaptureIds: Object.freeze([]),
  deliveryHandled: false,
});

function normalizeText(value) {
  return typeof value === "string" ? value.trim() : "";
}

function captureMeta(payload, relay = false) {
  const captureId = typeof payload.captureId === "number" && Number.isSafeInteger(payload.captureId)
    ? String(payload.captureId) : normalizeText(payload.captureId);
  return {
    captureId,
    utteranceId: normalizeText(payload.utteranceId || payload.voiceUtteranceId || (relay ? payload.sessionId : "")),
    boardDeviceId: normalizeText(payload.boardDeviceId),
    connectionId: normalizeText(payload.connectionId),
    bootId: normalizeText(payload.bootId),
  };
}

function captureForFlow(flow, action) {
  return Object.fromEntries(["captureId", "boardDeviceId", "connectionId", "bootId"]
    .map(key => [key, normalizeText(action[key]) || flow[key] || ""]));
}

// Business outcome, not diagnostic presence, determines presentation severity.
export function voiceFlowTone(flow) {
  if (flow?.phase === "submit_unknown") return "warning";
  if (flow?.phase === "done") return "success";
  if (flow?.phase === "error") return "error";
  return "muted";
}

export function voiceFlowMessage(flow) {
  const message = normalizeText(flow?.message);
  const error = normalizeText(flow?.composerError);
  const failed = flow?.phase === "error" || flow?.ok === false;
  if (!failed || !error || message.includes(error)) return message;
  return [message, `具体原因：${error}`].filter(Boolean).join("\n");
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
    retiredCaptureIds: state.flow.captureId
      ? [state.flow.captureId, ...(state.retiredCaptureIds || []).filter(id => id !== state.flow.captureId)]
        .slice(0, MAX_RETIRED_UTTERANCES)
      : state.retiredCaptureIds || [],
    deliveryHandled: false,
  };
}

function selectUtteranceState(state, action) {
  const utteranceId = normalizeText(action.utteranceId);
  const captureId = normalizeText(action.captureId);
  const phase = normalizeText(action.phase).toLowerCase();
  if (action.staleCapture || action.terminalAlreadyReported) {
    return { state, utteranceId, accepted: false };
  }
  if (captureId && (state.retiredCaptureIds || []).includes(captureId)) {
    return { state, utteranceId, accepted: false };
  }
  if (captureId && state.flow.captureId && captureId !== state.flow.captureId) {
    // Only a fresh begin/listening opens a new capture, never a late end/error.
    if (phase !== "listening" || action.terminalAlreadyReported) return { state, utteranceId, accepted: false };
    return { state: startUtterance(state, utteranceId), utteranceId, accepted: true };
  }
  for (const key of ["boardDeviceId", "connectionId", "bootId"]) {
    if (state.flow[key] && action[key] && state.flow[key] !== action[key]) {
      // A reused firmware utterance ID is not evidence of the same capture.
      if (!captureId || captureId === state.flow.captureId) return { state, utteranceId, accepted: false };
    }
  }
  if (state.flow.captureId && (!utteranceId || (!captureId && state.retiredUtteranceIds.includes(utteranceId)))) {
    return { state, utteranceId, accepted: false };
  }
  if (captureId && captureId === state.flow.captureId && utteranceId && state.activeUtteranceId
    && utteranceId !== state.activeUtteranceId) return { state, utteranceId, accepted: false };
  if (captureId && phase === "listening" && utteranceId && utteranceId !== state.activeUtteranceId) {
    return { state: startUtterance(state, utteranceId), utteranceId, accepted: true };
  }
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
    case "capture": {
      const selected = selectUtteranceState(state, action);
      if (!selected.accepted || TERMINAL_PHASES.has(selected.state.flow.phase)) return state;
      return { ...selected.state, activeUtteranceId: selected.utteranceId,
        flow: { ...selected.state.flow, ...captureForFlow(selected.state.flow, action),
          utteranceId: selected.utteranceId,
          phase: selected.state.flow.phase === "idle" ? "listening" : selected.state.flow.phase } };
    }
    case "transcript": {
      const selected = selectUtteranceState(state, action);
      if (!selected.accepted) return state;
      const baseState = selected.state;
      const flow = baseState.flow;
      if (TERMINAL_PHASES.has(flow.phase)) return state;
      if ((flow.isFinal || TERMINAL_PHASES.has(flow.phase))
          && phaseRank(action.phase || "listening") < phaseRank(flow.phase)) return state;
      const revision = Number(action.revision ?? flow.revision ?? 0);
      if (revision < Number(flow.revision || 0)) return state;
      const route = routeForFlow(flow, action);
      return {
        ...baseState,
        activeUtteranceId: selected.utteranceId,
        flow: {
          ...flow,
          ...captureForFlow(flow, action),
          phase: monotonicPhase(flow.phase, action.phase || "listening"),
          utteranceId: selected.utteranceId,
          revision,
          text: action.text ?? flow.text,
          message: action.message || flow.message || "",
          reply: "",
          ok: action.phase === "submit_unknown" ? false : action.ok,
          agentId: route.agentId,
          sessionId: route.sessionId,
          isFinal: flow.isFinal || action.isFinal === true,
          composerMode: action.composerMode || flow.composerMode || "",
          composerError: typeof action.composerError === "string" ? action.composerError : flow.composerError || "",
          updatedAt: Number(action.nowMs || Date.now()),
        },
      };
    }
    case "progress": {
      const selected = selectUtteranceState(state, action);
      if (!selected.accepted) return state;
      const baseState = selected.state;
      if (TERMINAL_PHASES.has(baseState.flow.phase)) return state;
      const route = routeForFlow(baseState.flow, action);
      return {
        ...baseState,
        activeUtteranceId: selected.utteranceId,
        flow: {
          ...baseState.flow,
          ...captureForFlow(baseState.flow, action),
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
      if (TERMINAL_PHASES.has(baseState.flow.phase)) return state;
      const terminal = action.pending !== true && action.transient !== true;
      if (terminal && baseState.deliveryHandled) return state;
      const phase = action.pending
        ? "waiting_reply"
        : action.transient
          ? "injecting"
          : action.code === "VISIBLE_COMPOSER_UNCONFIRMED"
            ? "submit_unknown"
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
          ...captureForFlow(baseState.flow, action),
          phase: monotonicPhase(baseState.flow.phase, phase),
          utteranceId: selected.utteranceId,
          text: action.text || baseState.flow.text || "",
          message: action.message || (action.ok ? "已发送到当前会话" : "发送失败"),
          reply: terminal ? action.reply || "" : "",
          ok: terminal ? phase !== "submit_unknown" && action.ok === true : null,
          agentId: route.agentId,
          sessionId: route.sessionId,
          composerMode: action.composerMode || baseState.flow.composerMode || "",
          composerError: terminal && action.ok === true
            && ["visible", "focused-input"].includes(action.composerMode)
            ? ""
            : typeof action.composerError === "string" ? action.composerError : baseState.flow.composerError || "",
          updatedAt: Number(action.nowMs || Date.now()),
        },
      };
    }
    default:
      return state;
  }
}

export function transcriptMessage(payload, phase, composerMode) {
  const visibleAgentLabel = normalizeText(payload?.agentId).toLowerCase() === "deepseek-harness" ? "DeepSeek Harness" : normalizeText(payload?.agentId).toLowerCase() === "workbuddy" ? "WorkBuddy" : normalizeText(payload?.agentId).toLowerCase() === "claude-code"
    ? "Claude"
    : "ChatGPT（Codex）";
  if (phase === "listening") {
    if (["WorkBuddy", "DeepSeek Harness"].includes(visibleAgentLabel)) {
      return `正在聆听；松开后将完整识别结果一次追加到 ${visibleAgentLabel} 输入框，不自动发送。`;
    }
    if (composerMode === "visible") {
      return `正在聆听，识别文字会实时同步到 ${visibleAgentLabel} 输入框。`;
    }
    if (composerMode === "focused-input") {
      return "正在聆听；松开后只把文字写入 MiMoCode 当前光标。";
    }
    return "正在聆听并实时识别。";
  }
  if (phase === "partial") {
    if (["WorkBuddy", "DeepSeek Harness"].includes(visibleAgentLabel)) {
      return `正在实时识别；松开后一次写入 ${visibleAgentLabel}，写入确认前请保持目标对话在前台。`;
    }
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
      return "语音文字已写入 MiMoCode，不会自动发送。";
    }
    if (composerMode === "agent-bus") {
      return "语音文字已识别为待发送草稿，不会自动发送。";
    }
    return `语音文字已追加到 ${visibleAgentLabel} 输入框，不会自动发送。`;
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
  if (phase === "submit_unknown") {
    return "已尝试提交，发送结果未确认；请核对目标对话，避免重复发送。";
  }
  if (phase === "error") {
    return normalizeText(payload.error) || "设备语音处理失败。";
  }
  return "";
}

function audioActivity(payload) {
  if (payload.failureDomain === "delivery") return null;
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

// Independent of React listener reinstallation and transcript delivery order.
// A valid USB end is only transport completion, not ASR/delivery success: its
// first later recognition error must still be displayed. After an error or
// cancellation, no late end/duplicate may replace that cause.
export function createDeviceAudioActivityGate() {
  let state = DEVICE_VOICE_ROUTER_INITIAL_STATE;
  let endedCapture = "";
  return (payload) => {
    if (payload.failureDomain === "delivery") return null;
    const phase = normalizeText(payload.phase).toLowerCase();
    if (!phase || phase === "status" || phase === "configured") return null;
    const meta = captureMeta(payload, true);
    const failed = payload.ok === false || phase === "cancelled";
    const begin = !failed && ["begin", "listening"].includes(phase);
    const action = {
      ...meta,
      type: begin ? "capture" : "transcript",
      phase: failed ? (phase === "cancelled" ? "cancelled" : "error")
        : ["begin", "streaming"].includes(phase) ? "listening"
          : phase === "end" ? "recognizing" : phase,
      ok: !failed,
      staleCapture: payload.staleCapture === true,
      terminalAlreadyReported: payload.terminalAlreadyReported === true,
      message: failed ? normalizeText(payload.error) || "音频流处理失败" : "",
      isFinal: payload.isFinal === true,
    };
    const next = deviceVoiceRouterReducer(state, action);
    if (next === state) return null;
    state = next;
    const activity = audioActivity(payload);
    if (!activity || (!failed && payload.duplicate === true)) return null;
    const key = state.flow.captureId || state.activeUtteranceId;
    if (phase === "end" && !failed) {
      if (endedCapture === key) return null;
      endedCapture = key;
    }
    return { ...activity, ...captureForFlow(state.flow, meta), utteranceId: state.activeUtteranceId };
  };
}

export function useDeviceVoiceRouter({ onAudioActivity } = {}) {
  const [routerState, dispatch] = useReducer(
    deviceVoiceRouterReducer,
    DEVICE_VOICE_ROUTER_INITIAL_STATE,
  );
  const audioActivityGateRef = useRef(null);
  if (!audioActivityGateRef.current) audioActivityGateRef.current = createDeviceAudioActivityGate();

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
          ...captureMeta(payload),
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
          ...captureMeta(payload),
          type: "delivery",
          utteranceId: normalizeText(payload.utteranceId || payload.voiceUtteranceId),
          ok,
          transient,
          pending,
          code: normalizeText(payload.code),
          text: normalizeText(payload.text),
          message: !ok && transient
            ? `桥接连接瞬时抖动，正在确认是否已送达...\n${baseMessage}`
            : baseMessage,
          reply,
          agentId,
          sessionId,
          composerMode,
          composerError: typeof payload.composerError === "string"
            ? normalizeText(payload.composerError)
            : !ok && typeof payload.error === "string" ? normalizeText(payload.error) : undefined,
        });
      });

      unlistenUsbAudio = await listen("usb-audio-stream", (event) => {
        const payload = event?.payload && typeof event.payload === "object" ? event.payload : {};
        if (payload.failureDomain === "delivery") return;
        const meta = captureMeta(payload, true);
        if (payload.phase === "begin" && payload.ok !== false) {
          dispatch({ ...meta, type: "capture", phase: "listening", ok: true,
            staleCapture: payload.staleCapture === true, terminalAlreadyReported: payload.terminalAlreadyReported === true });
        } else if (payload.ok === false || payload.phase === "cancelled") {
          dispatch({ ...meta, type: "transcript", phase: payload.phase === "cancelled" ? "cancelled" : "error",
            ok: false, message: normalizeText(payload.error) || "音频流处理失败",
            staleCapture: payload.staleCapture === true, terminalAlreadyReported: payload.terminalAlreadyReported === true });
        }
        const activity = audioActivityGateRef.current(payload);
        if (activity) onAudioActivity?.(activity);
      });

      unlistenVoiceTranscript = await listen("voice-transcript", (event) => {
        const payload = event?.payload && typeof event.payload === "object" ? event.payload : {};
        const phase = normalizeText(payload.phase).toLowerCase() || "listening";
        const composerMode = normalizeText(payload.composerMode).toLowerCase();
        dispatch({
          ...captureMeta(payload),
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
          composerError: typeof payload.composerError === "string"
            ? normalizeText(payload.composerError) : undefined,
          staleCapture: payload.staleCapture === true,
          terminalAlreadyReported: payload.terminalAlreadyReported === true,
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
