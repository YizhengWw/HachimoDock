/**
 * [Input] Followed Agent id, conversation visibility, dismissed-card signatures, and local Agent Bus responses.
 * [Output] Agent-isolated routing Sessions plus the device-visible active/terminal queue.
 * [Pos] Persistent dashboard Session polling and reducer boundary.
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md` and `ref/src/.folder.md`.
 */

import { useEffect, useReducer, useRef } from "react";
import {
  fetchAgentSessionEvents,
  fetchAgentSessions,
} from "../lib/agent-bus-client.js";
import {
  buildP4ConversationQueue,
  filterDismissedP4Sessions,
  mergeP4SessionEvent,
  mergeP4SessionSnapshot,
} from "../lib/p4-session-service.js";

export const P4_SESSION_EVENT_POLL_MS = 750;
export const P4_SESSION_SNAPSHOT_POLL_MS = 5000;
export const P4_SESSION_EVENT_DISPATCH_GAP_MS = 25;

const TERMINAL_SESSION_STATES = new Set([
  "done",
  "error",
  "failed",
  "complete",
  "completed",
]);
const EMPTY_SESSIONS = Object.freeze([]);

export const AGENT_SESSION_FEED_INITIAL_STATE = Object.freeze({
  sessions: EMPTY_SESSIONS,
  routingSessions: EMPTY_SESSIONS,
  fingerprint: "",
  loaded: false,
  agentId: "",
});

function normalizeText(value) {
  return typeof value === "string" ? value.trim() : "";
}

function nextSessionFeedState(state, action, routingSessions, authoritativeSnapshot) {
  const agentId = action.agentId || "";
  const sameAgent = state.loaded && state.agentId === agentId;
  const displayEnabled = action.displayEnabled !== false;
  const dismissedSessions = action.dismissedSessions || {};
  const previousQueue = displayEnabled && sameAgent
    ? filterDismissedP4Sessions(state.sessions, dismissedSessions)
    : [];
  const visibleRoutingSessions = filterDismissedP4Sessions(
    routingSessions,
    dismissedSessions,
  );
  const sessions = displayEnabled
    ? buildP4ConversationQueue(
      visibleRoutingSessions,
      previousQueue,
      action.nowMs,
      { authoritativeSnapshot },
    )
    : [];
  const fingerprint = JSON.stringify([sessions, routingSessions]);
  if (sameAgent && fingerprint === state.fingerprint) return state;
  return {
    sessions,
    routingSessions,
    fingerprint,
    loaded: true,
    agentId,
  };
}

export function agentSessionFeedReducer(state, action) {
  switch (action.type) {
    case "replace_snapshot": {
      const agentId = action.agentId || "";
      const sameAgent = state.loaded && state.agentId === agentId;
      const routingSessions = mergeP4SessionSnapshot(
        sameAgent ? state.routingSessions : [],
        action.value,
      );
      return nextSessionFeedState(state, action, routingSessions, true);
    }
    case "apply_event": {
      const agentId = action.agentId || "";
      const sameAgent = state.loaded && state.agentId === agentId;
      const routingSessions = mergeP4SessionEvent(
        sameAgent ? state.routingSessions : [],
        action.value,
      );
      return nextSessionFeedState(state, action, routingSessions, false);
    }
    case "tick_terminal": {
      const agentId = action.agentId || "";
      if (!state.loaded || state.agentId !== agentId) return state;
      const sessions = action.displayEnabled === false
        ? []
        : buildP4ConversationQueue(
          filterDismissedP4Sessions(state.routingSessions, action.dismissedSessions),
          filterDismissedP4Sessions(state.sessions, action.dismissedSessions),
          action.nowMs,
        );
      const fingerprint = JSON.stringify([sessions, state.routingSessions]);
      if (fingerprint === state.fingerprint) return state;
      return { ...state, sessions, fingerprint };
    }
    case "reset":
      return AGENT_SESSION_FEED_INITIAL_STATE;
    default:
      return state;
  }
}

export function useAgentSessionFeed({ agentId, displayEnabled, dismissedSessions }) {
  const [state, dispatch] = useReducer(
    agentSessionFeedReducer,
    AGENT_SESSION_FEED_INITIAL_STATE,
  );
  const previousAgentIdRef = useRef("");
  const terminalQueuePendingRef = useRef(false);
  const matchesAgent = state.agentId === agentId;
  const sessions = matchesAgent ? state.sessions : EMPTY_SESSIONS;
  const routingSessions = matchesAgent ? state.routingSessions : EMPTY_SESSIONS;
  const loaded = matchesAgent && state.loaded;

  terminalQueuePendingRef.current = sessions.some((session) => (
    TERMINAL_SESSION_STATES.has(normalizeText(session?.state).toLowerCase())
  ));

  useEffect(() => {
    if (previousAgentIdRef.current === agentId) return;
    previousAgentIdRef.current = agentId;
    dispatch({ type: "reset" });
  }, [agentId]);

  useEffect(() => {
    if (!agentId) return undefined;

    let cancelled = false;
    let eventCursor;
    let eventStreamId = "";
    let eventsInFlight = false;
    let snapshotInFlight = false;
    let eventTimer = null;
    let snapshotTimer = null;
    let drainTimer = null;
    let draining = false;
    const pendingEvents = [];
    const ctl = new AbortController();

    const drainEventQueue = () => {
      if (cancelled) {
        draining = false;
        return;
      }
      const event = pendingEvents.shift();
      if (!event) {
        draining = false;
        return;
      }
      if (event.session && normalizeText(event.session.id)) {
        dispatch({
          type: "apply_event",
          agentId,
          value: event.session,
          nowMs: Number(event.updatedAt || event.session.statusUpdatedAt || Date.now()),
          displayEnabled,
          dismissedSessions,
        });
      }
      drainTimer = setTimeout(() => {
        drainTimer = null;
        drainEventQueue();
      }, P4_SESSION_EVENT_DISPATCH_GAP_MS);
    };

    const enqueueEvents = (events) => {
      pendingEvents.push(...(Array.isArray(events) ? events : []));
      if (draining || pendingEvents.length === 0) return;
      draining = true;
      drainEventQueue();
    };

    const pollEvents = async () => {
      if (cancelled || eventsInFlight) return;
      eventsInFlight = true;
      try {
        const result = await fetchAgentSessionEvents(
          agentId,
          eventCursor,
          eventStreamId,
          ctl.signal,
        );
        if (cancelled) return;
        const streamChanged = Boolean(
          eventStreamId && result.streamId && eventStreamId !== result.streamId,
        );
        if (result.reset || streamChanged) {
          pendingEvents.length = 0;
          // The replacement stream bootstraps active Sessions immediately.
          // Keep the last rendered queue until those events arrive so a
          // bridge restart cannot flash every device card off and back on.
        }
        eventCursor = result.cursor;
        eventStreamId = result.streamId;
        enqueueEvents(result.events);
      } catch {
        // Keep the cursor and retry. A successful response resumes or resets it.
      } finally {
        eventsInFlight = false;
        if (!cancelled && terminalQueuePendingRef.current) {
          dispatch({
            type: "tick_terminal",
            agentId,
            nowMs: Date.now(),
            displayEnabled,
            dismissedSessions,
          });
        }
      }
    };

    const refreshSnapshot = async () => {
      if (cancelled || snapshotInFlight) return;
      snapshotInFlight = true;
      try {
        const nextSessions = await fetchAgentSessions(agentId, ctl.signal);
        if (!cancelled) {
          dispatch({
            type: "replace_snapshot",
            agentId,
            value: nextSessions,
            nowMs: Date.now(),
            displayEnabled,
            dismissedSessions,
          });
        }
      } catch {
        // Keep the last confirmed list while event delivery continues.
      } finally {
        snapshotInFlight = false;
      }
    };

    const scheduleEventPoll = () => {
      eventTimer = setTimeout(async () => {
        await pollEvents();
        if (!cancelled) scheduleEventPoll();
      }, P4_SESSION_EVENT_POLL_MS);
    };
    const scheduleSnapshotRefresh = () => {
      snapshotTimer = setTimeout(async () => {
        await refreshSnapshot();
        if (!cancelled) scheduleSnapshotRefresh();
      }, P4_SESSION_SNAPSHOT_POLL_MS);
    };

    const bootstrap = async () => {
      await pollEvents();
      await refreshSnapshot();
      await pollEvents();
      if (cancelled) return;
      scheduleEventPoll();
      scheduleSnapshotRefresh();
    };
    bootstrap();

    return () => {
      cancelled = true;
      ctl.abort();
      pendingEvents.length = 0;
      if (eventTimer) clearTimeout(eventTimer);
      if (snapshotTimer) clearTimeout(snapshotTimer);
      if (drainTimer) clearTimeout(drainTimer);
    };
  }, [agentId, dismissedSessions, displayEnabled]);

  return { sessions, routingSessions, loaded };
}
