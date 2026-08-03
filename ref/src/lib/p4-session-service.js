/**
 * [Input] Ordered Agent session snapshots/events and the previous device-visible queue.
 * [Output] Agent-isolated P4 routing sessions, stable 60-second terminal cards, and bounded transport payloads.
 * [Pos] Pure P4 Session service shared by the persistent dashboard orchestrator.
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import {
  DEVICE_SESSION_TERMINAL_HOLD_MS,
  isActiveDeviceSession,
  reconcileDeviceSessionQueue,
} from "./session-display.js";

const P4_DEVICE_SESSION_LIMIT = 8;

function normalizeSessionText(value) {
  return typeof value === "string" ? value.trim() : "";
}

const GENERIC_SESSION_TITLES = new Set([
  "会话",
  "Claude 会话",
  "Codex 会话",
  "MiMoCode 会话",
  "OpenClaw 会话",
]);

function isInternalReviewSession(session) {
  const model = normalizeSessionText(session?.model).toLowerCase();
  if (["codex-auto-review", "codex_auto_review", "approvals-reviewer"].includes(model)) {
    return true;
  }
  const text = [
    session?.name,
    session?.summary,
    session?.displayTitle,
    session?.displayContent,
  ].map(normalizeSessionText).filter(Boolean).join(" ").toLowerCase();
  const schemaMarkerCount = ["risk_level", "user_authorization", "outcome", "rationale"]
    .filter((marker) => text.includes(marker)).length;
  return schemaMarkerCount >= 3 && text.includes("the following is the code");
}

export function p4SessionDisplayTitle(session) {
  const candidates = [session?.name, session?.displayTitle, session?.summary]
    .map(normalizeSessionText)
    .filter((title) => title && !title.startsWith("<recommended_plugins>"));
  return candidates.find((title) => !GENERIC_SESSION_TITLES.has(title))
    || candidates[0]
    || "";
}

export function p4SessionHasMeaningfulTitle(session) {
  const title = p4SessionDisplayTitle(session);
  return Boolean(title && !GENERIC_SESSION_TITLES.has(title));
}

export function buildP4RoutingSessions(sessions) {
  const sessionsById = new Map();
  for (const session of Array.isArray(sessions) ? sessions : []) {
    if (isInternalReviewSession(session)) continue;
    const id = normalizeSessionText(session?.id);
    if (!id) continue;
    const existing = sessionsById.get(id);
    if (!existing || Number(session?.lastModified || 0) > Number(existing?.lastModified || 0)) {
      sessionsById.set(id, session);
    }
  }
  const recency = (session) => Math.max(
    Number(session?.statusUpdatedAt || 0),
    Number(session?.lastModified || 0),
  );
  return [...sessionsById.values()]
    .sort((a, b) => recency(b) - recency(a));
}

export function mergeP4SessionSnapshot(previousSessions, incomingSessions) {
  const previousById = new Map(
    buildP4RoutingSessions(previousSessions)
      .map((session) => [normalizeSessionText(session?.id), session]),
  );
  const merged = (Array.isArray(incomingSessions) ? incomingSessions : []).map((session) => {
    const id = normalizeSessionText(session?.id);
    const previous = id ? previousById.get(id) : null;
    if (!previous) return session;
    const next = { ...previous, ...session, id };
    const incomingState = normalizeSessionText(session?.state);
    if (!incomingState && normalizeSessionText(previous?.state)) {
      next.state = previous.state;
      next.statusUpdatedAt = Math.max(
        Number(previous?.statusUpdatedAt || 0),
        Number(session?.statusUpdatedAt || 0),
      );
      for (const key of ["displayTitle", "displayContent"]) {
        if (!normalizeSessionText(session?.[key]) && normalizeSessionText(previous?.[key])) {
          next[key] = previous[key];
        }
      }
    }
    for (const key of ["name", "summary", "cwd"]) {
      if (!normalizeSessionText(session?.[key]) && normalizeSessionText(previous?.[key])) {
        next[key] = previous[key];
      }
    }
    next.lastModified = Math.max(
      Number(previous?.lastModified || 0),
      Number(session?.lastModified || 0),
      Number(next.statusUpdatedAt || 0),
    );
    return next;
  });
  return buildP4RoutingSessions(merged);
}

export function mergeP4SessionEvent(previousSessions, incomingSession) {
  const id = normalizeSessionText(incomingSession?.id);
  if (!id) return buildP4RoutingSessions(previousSessions);
  const previous = (Array.isArray(previousSessions) ? previousSessions : [])
    .find((session) => normalizeSessionText(session?.id) === id);
  const statusUpdatedAt = Number(incomingSession?.statusUpdatedAt || 0);
  const merged = {
    ...(previous || {}),
    ...incomingSession,
    id,
    lastModified: Math.max(
      Number(previous?.lastModified || 0),
      Number(incomingSession?.lastModified || 0),
      statusUpdatedAt,
    ),
  };
  return buildP4RoutingSessions([
    merged,
    ...(Array.isArray(previousSessions) ? previousSessions : [])
      .filter((session) => normalizeSessionText(session?.id) !== id),
  ]);
}

export function buildP4ConversationQueue(
  sessions,
  previousQueue = [],
  nowMs = Date.now(),
  { authoritativeSnapshot = false } = {},
) {
  const eligibleSessions = buildP4RoutingSessions(sessions).filter((session) => (
    !isActiveDeviceSession(session) || p4SessionHasMeaningfulTitle(session)
  ));
  return reconcileDeviceSessionQueue(
    previousQueue,
    eligibleSessions,
    nowMs,
    P4_DEVICE_SESSION_LIMIT,
    { authoritativeSnapshot },
  );
}

export function buildP4DeviceSessionTransportPayload(sessions, nowMs = Date.now()) {
  const numericNow = Number(nowMs);
  const currentTime = Number.isFinite(numericNow) ? numericNow : Date.now();
  return (Array.isArray(sessions) ? sessions : []).map((session) => {
    const { terminalUntilMs, ...payload } = session;
    return {
      ...payload,
      terminalRemainingMs: Math.min(
        DEVICE_SESSION_TERMINAL_HOLD_MS,
        Math.max(0, Math.floor(Number(terminalUntilMs || 0) - currentTime)),
      ),
    };
  });
}

export function p4SessionActivitySignature(session) {
  return JSON.stringify([
    Number(session?.lastModified || 0),
    Number(session?.statusUpdatedAt || 0),
    normalizeSessionText(session?.state),
    normalizeSessionText(session?.displayContent),
    normalizeSessionText(session?.summary),
  ]);
}

export function filterDismissedP4Sessions(sessions, dismissedSessions = {}) {
  return (Array.isArray(sessions) ? sessions : []).filter((session) => {
    const id = normalizeSessionText(session?.id);
    return !id || dismissedSessions[id] !== p4SessionActivitySignature(session);
  });
}
