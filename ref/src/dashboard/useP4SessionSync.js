/**
 * [Input] Followed Agent Session feed, P4 identity/connection, visibility policy, and dashboard notifications.
 * [Output] Exact visible-card selection, foreground-voice lease reconciliation, temporary routing target, encoder handling, and serialized P4 Session downlinks.
 * [Pos] Dashboard-to-device Session synchronization boundary.
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md` and `ref/src/.folder.md`.
 */

import { useCallback, useEffect, useMemo, useReducer, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import {
  deviceSessionTerminalUntilMs,
  deviceSessionTransitionRevision,
  filterActiveDeviceSessions,
} from "../lib/session-display.js";
import {
  buildP4DeviceSessionTransportPayload,
  p4SessionDisplayTitle,
  p4SessionActivitySignature,
} from "../lib/p4-session-service.js";

export const P4_MANUAL_SESSION_TIMEOUT_MS = 5 * 60_000;
export const P4_SESSION_LEASE_REFRESH_MS = 4_000;
const P4_DEVICE_SESSION_LIMIT = 8;

function normalizeText(value) {
  return typeof value === "string" ? value.trim() : "";
}

function clampUtf8Text(value, maxBytes) {
  let output = "";
  for (const char of String(value || "")) {
    if (new TextEncoder().encode(output + char).length > maxBytes) break;
    output += char;
  }
  return output;
}

export function cycleVoiceSessionId(currentSessionId, sessions, direction) {
  const sessionIds = [...new Set((Array.isArray(sessions) ? sessions : [])
    .map((session) => normalizeText(session?.id))
    .filter(Boolean))];
  if (!sessionIds.length) return "auto";
  const normalizedCurrent = normalizeText(currentSessionId);
  const currentIndex = normalizedCurrent && normalizedCurrent !== "auto"
    ? sessionIds.indexOf(normalizedCurrent)
    : 0;
  if (currentIndex < 0) return sessionIds[0];
  if (sessionIds.length === 1) return sessionIds[0];
  return sessionIds[
    (currentIndex + (direction < 0 ? -1 : 1) + sessionIds.length) % sessionIds.length
  ];
}

export function formatDeviceSessionTitle(session, fallbackSessionId = "") {
  const title = p4SessionDisplayTitle(session);
  if (title) return title;
  const sessionId = normalizeText(session?.id) || normalizeText(fallbackSessionId);
  return sessionId && sessionId !== "auto" ? `会话 ${sessionId.slice(0, 8)}` : "";
}

export function isDeviceSessionTargetUnique(selectedSession, sessions) {
  const selectedTitle = formatDeviceSessionTitle(selectedSession);
  if (!selectedTitle) return false;
  const titleMatches = (Array.isArray(sessions) ? sessions : []).filter(
    (session) => formatDeviceSessionTitle(session) === selectedTitle,
  );
  if (titleMatches.length <= 1) return true;
  const selectedCwd = normalizeText(selectedSession?.cwd);
  if (!selectedCwd) return false;
  return titleMatches.filter(
    (session) => normalizeText(session?.cwd) === selectedCwd,
  ).length === 1;
}

export function formatDeviceSessionContent(session, title = "") {
  const liveContent = normalizeText(session?.displayContent);
  if (liveContent) return clampUtf8Text(liveContent, 383);
  const summary = normalizeText(session?.summary);
  if (!summary || summary.startsWith("<recommended_plugins>")) return "";
  if (summary === normalizeText(title)) return "";
  return clampUtf8Text(summary, 383);
}

export function useP4SessionSync({
  enabled,
  boardDeviceId,
  agentId,
  usbConnected,
  displayEnabled,
  sessions,
  routingSessions,
  sessionsLoaded,
  dismissedSessions,
  onDismissSessions,
  formatSessionOption,
  push,
}) {
  const [selection, setSelection] = useState({ sessionId: "auto", selectedAt: 0 });
  const [bindingSyncRevision, requestBindingSync] = useReducer(
    (revision) => revision + 1,
    0,
  );
  const pendingNoticeRef = useRef(null);
  const bindingQueueRef = useRef(Promise.resolve());
  const sessionId = selection.sessionId;

  const visibleSessions = useMemo(() => (
    (Array.isArray(sessions) ? sessions : []).filter((session) => {
      const id = normalizeText(session?.id);
      return !id || dismissedSessions?.[id] !== p4SessionActivitySignature(session);
    })
  ), [dismissedSessions, sessions]);
  const selectableSessions = useMemo(
    () => displayEnabled ? visibleSessions : [],
    [displayEnabled, visibleSessions],
  );
  const switchCandidates = selectableSessions;
  const autoRoutingSession = selectableSessions[0] || routingSessions?.[0] || sessions?.[0];
  const selectedRoutingSession = (sessionId === "auto"
    ? autoRoutingSession
    : (routingSessions || []).find(
      (session) => normalizeText(session?.id) === normalizeText(sessionId),
    )) || null;
  const selectedVisibleSession = sessionId === "auto"
    ? (selectableSessions[0] || null)
    : selectableSessions.find(
      (session) => normalizeText(session?.id) === normalizeText(sessionId),
    ) || null;
  const selectedTargetSession = selectedRoutingSession || selectedVisibleSession;
  const selectedSessionTitle = selectedTargetSession
    ? formatDeviceSessionTitle(selectedTargetSession, sessionId)
    : "";
  const selectedDeviceSessionTitle = selectedVisibleSession
    ? formatDeviceSessionTitle(selectedVisibleSession, sessionId)
    : "";
  const selectedSessionCwd = normalizeText(selectedTargetSession?.cwd);
  const selectedSessionTitleUnique = isDeviceSessionTargetUnique(
    selectedTargetSession,
    routingSessions,
  );
  const selectedSessionIndex = selectedVisibleSession
    ? selectableSessions.findIndex((session) => session === selectedVisibleSession) + 1
    : 0;
  const deviceSessions = useMemo(() => selectableSessions.map((session) => {
    const title = formatDeviceSessionTitle(session);
    return {
      id: clampUtf8Text(normalizeText(session?.id), 127),
      title: clampUtf8Text(title, 191),
      cwd: clampUtf8Text(normalizeText(session?.cwd), 2048),
      content: formatDeviceSessionContent(session, title),
      state: normalizeText(session?.state).toLowerCase() || "idle",
      transitionRevision: deviceSessionTransitionRevision(session),
      terminalUntilMs: deviceSessionTerminalUntilMs(session),
    };
  }), [selectableSessions]);
  const deviceSessionsSignature = JSON.stringify(deviceSessions);
  const activeSessionIds = useMemo(
    () => filterActiveDeviceSessions(selectableSessions)
      .slice(0, P4_DEVICE_SESSION_LIMIT)
      .map((session) => clampUtf8Text(normalizeText(session?.id), 127))
      .filter(Boolean),
    [selectableSessions],
  );
  const activeSessionIdsSignature = JSON.stringify(activeSessionIds);
  const cardsEnabled = displayEnabled && deviceSessions.length > 0;

  const onSessionChange = useCallback((nextSessionId) => {
    const value = nextSessionId || "auto";
    setSelection({
      sessionId: value,
      selectedAt: value === "auto" ? 0 : Date.now(),
    });
  }, []);

  useEffect(() => {
    setSelection({ sessionId: "auto", selectedAt: 0 });
    pendingNoticeRef.current = null;
  }, [agentId, boardDeviceId]);

  useEffect(() => {
    if (!enabled || !boardDeviceId || !agentId) return undefined;
    const timer = setInterval(requestBindingSync, P4_SESSION_LEASE_REFRESH_MS);
    return () => clearInterval(timer);
  }, [agentId, boardDeviceId, enabled]);

  useEffect(() => {
    if (sessionId === "auto") return undefined;
    const selectedStillExists = switchCandidates.some(
      (session) => normalizeText(session?.id) === normalizeText(sessionId),
    );
    if (!selectedStillExists) {
      onSessionChange("auto");
      return undefined;
    }
    const elapsed = Date.now() - selection.selectedAt;
    const delay = Math.max(0, P4_MANUAL_SESSION_TIMEOUT_MS - elapsed);
    const timer = setTimeout(() => onSessionChange("auto"), delay);
    return () => clearTimeout(timer);
  }, [onSessionChange, selection.selectedAt, sessionId, switchCandidates]);

  useEffect(() => {
    if (!enabled || !boardDeviceId || !agentId) return;
    const pendingNotice = pendingNoticeRef.current;
    const matchedNotice = pendingNotice?.sessionId === sessionId ? pendingNotice : null;
    const notice = matchedNotice?.message || "";
    const confirmedEmptyQueue = sessionsLoaded && selectableSessions.length === 0;
    if (
      ["codex", "claude-code"].includes(agentId)
      && !selectedRoutingSession
      && !confirmedEmptyQueue
    ) return;
    const autoFollow = sessionId === "auto" && Boolean(selectedRoutingSession);
    const requestedSessionId = autoFollow
      ? normalizeText(selectedRoutingSession?.id)
      : sessionId;
    if (matchedNotice) pendingNoticeRef.current = null;
    const bindingPromise = bindingQueueRef.current
      .catch(() => undefined)
      .then(() => invoke("set_p4_session_binding", {
        input: {
          boardDeviceId,
          agentId,
          sessionId: requestedSessionId,
          autoFollow,
          sessionTitle: selectedSessionTitle,
          deviceTitle: selectedDeviceSessionTitle,
          sessionCwd: selectedSessionCwd,
          sessionTitleUnique: selectedSessionTitleUnique,
          locateDesktop: Boolean(matchedNotice),
          sessionIndex: selectedSessionIndex,
          sessionCount: selectableSessions.length,
          sessions: buildP4DeviceSessionTransportPayload(deviceSessions),
          activeSessionIds,
          displayEnabled: cardsEnabled,
          notice,
        },
      }));
    bindingQueueRef.current = bindingPromise.catch(() => undefined);
    bindingPromise
      .then((result) => {
        const resultSessionId = normalizeText(result?.sessionId) || "auto";
        if (resultSessionId !== requestedSessionId || !matchedNotice) return;
        const location = normalizeText(result?.desktopLocation).toLowerCase();
        const locationError = normalizeText(result?.desktopLocationError);
        const desktopAgentLabel = agentId === "claude-code" ? "Claude" : "ChatGPT（Codex）";
        if (["codex", "claude-code"].includes(agentId) && location !== "located") {
          push?.({
            tone: "warning",
            title: `会话已切换，${desktopAgentLabel} 定位失败`,
            message: locationError || matchedNotice.sessionLabel,
          });
          return;
        }
        push?.({
          tone: "success",
          title: ["codex", "claude-code"].includes(agentId)
            ? `${matchedNotice.title}并定位`
            : matchedNotice.title,
          message: matchedNotice.sessionLabel,
        });
      })
      .catch((error) => {
        console.warn("[p4] failed to update target session", error);
        if (matchedNotice) {
          push?.({ tone: "error", title: "会话切换失败", message: String(error) });
        }
      });
  }, [
    activeSessionIdsSignature,
    agentId,
    bindingSyncRevision,
    boardDeviceId,
    cardsEnabled,
    deviceSessionsSignature,
    enabled,
    push,
    selectableSessions.length,
    selectedDeviceSessionTitle,
    selectedRoutingSession,
    selectedSessionCwd,
    selectedSessionIndex,
    selectedSessionTitle,
    selectedSessionTitleUnique,
    sessionId,
    sessionsLoaded,
    usbConnected,
  ]);

  useEffect(() => {
    if (!enabled || !boardDeviceId || !agentId) return undefined;
    let disposed = false;
    let unlistenSessionSwitch = null;
    let unlistenCurrentVoice = null;

    const setup = async () => {
      unlistenSessionSwitch = await listen("usb-message", (event) => {
        const envelope = event?.payload && typeof event.payload === "object"
          ? event.payload
          : {};
        if (envelope.topic !== "input/event") return;
        const payload = envelope.payload && typeof envelope.payload === "object"
          ? envelope.payload
          : {};
        if (normalizeText(payload.boardDeviceId) !== boardDeviceId) return;
        const action = normalizeText(payload.action);
        if (action === "session_clear") {
          const dismissed = {};
          for (const session of sessions || []) {
            const id = normalizeText(session?.id);
            if (id) dismissed[id] = p4SessionActivitySignature(session);
          }
          pendingNoticeRef.current = null;
          onDismissSessions?.(dismissed);
          push?.({
            tone: "success",
            title: "主页会话已清空",
            message: "新会话或新活动会自动重新显示。",
          });
          return;
        }
        const direction = action === "session_next"
          ? 1
          : action === "session_previous"
            ? -1
            : 0;
        if (!direction) return;

        const boardSelectedSessionId = normalizeText(payload.sessionId);
        const boardSelectedSession = boardSelectedSessionId
          ? switchCandidates.find(
            (session) => normalizeText(session?.id) === boardSelectedSessionId,
          )
          : null;
        const nextSessionId = boardSelectedSession
          ? boardSelectedSessionId
          : cycleVoiceSessionId(sessionId, switchCandidates, direction);
        if (nextSessionId === "auto") {
          push?.({
            tone: "warning",
            title: "没有可切换的会话",
            message: "请等待当前 Agent 的会话列表加载完成。",
          });
          return;
        }
        pendingNoticeRef.current = {
          sessionId: nextSessionId,
          message: direction > 0 ? "已切换到下一个会话" : "已切换到上一个会话",
          title: direction > 0 ? "已切换到下一个会话" : "已切换到上一个会话",
          sessionLabel: formatSessionOption?.(
            boardSelectedSession || switchCandidates.find(
              (session) => normalizeText(session?.id) === nextSessionId,
            ),
          ) || "",
        };
        onSessionChange(nextSessionId);
        requestBindingSync();
      });
      unlistenCurrentVoice = await listen("voice-transcript", (event) => {
        const payload = event?.payload && typeof event.payload === "object"
          ? event.payload
          : {};
        if (normalizeText(payload.boardDeviceId) !== boardDeviceId) return;
        if (normalizeText(payload.agentId) !== agentId) return;
        if (normalizeText(payload.phase).toLowerCase() !== "submitted") return;
        if (normalizeText(payload.sessionId).toLowerCase() !== "current") return;

        // The foreground Agent session has just received the confirmed turn.
        // Release a stale manual device-card lease so the refreshed newest
        // session becomes the selected bubble instead of jumping back later.
        pendingNoticeRef.current = null;
        onSessionChange("auto");
        requestBindingSync();
      });
      if (disposed) {
        unlistenSessionSwitch?.();
        unlistenCurrentVoice?.();
      }
    };

    setup().catch((error) => {
      console.warn("[device] failed to listen for session or voice routing events", error);
    });
    return () => {
      disposed = true;
      unlistenSessionSwitch?.();
      unlistenCurrentVoice?.();
    };
  }, [
    agentId,
    boardDeviceId,
    enabled,
    formatSessionOption,
    onDismissSessions,
    onSessionChange,
    push,
    sessionId,
    sessions,
    switchCandidates,
  ]);

  return {
    sessionId,
    sessions: Array.isArray(sessions) ? sessions : [],
    routingSessions: Array.isArray(routingSessions) ? routingSessions : [],
    sessionsLoaded,
    selectedRoutingSession,
    selectableSessions,
    onSessionChange,
  };
}
