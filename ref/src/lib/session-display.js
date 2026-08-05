/**
 * [Input] Persisted global visibility, ordered Agent lifecycle events plus periodic snapshots, and an optional storage adapter.
 * [Output] Cross-Agent show/hide plus explicit-transition active/terminal device-card visibility that tolerates incomplete snapshots and transient duplicate identities.
 * [Pos] shared dashboard configuration helper in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export const SESSION_DISPLAY_ENABLED_STORAGE_KEY = "pet-manager.session-display-enabled";
export const DEFAULT_SESSION_DISPLAY_ENABLED = true;
export const DEVICE_SESSION_TERMINAL_HOLD_MS = 60_000;
export const DEVICE_SESSION_SNAPSHOT_MISSING_GRACE_MS = 15_000;
export const ACTIVE_DEVICE_SESSION_STATES = Object.freeze([
  "working",
  "thinking",
  "tool_running",
  "speaking",
  "waiting_user",
]);
export const TERMINAL_DEVICE_SESSION_STATES = Object.freeze(["done", "error"]);

const LEGACY_CODEX_SESSION_DISPLAY_ENABLED_STORAGE_KEY =
  "pet-manager.codex-session-display-enabled";
const DEVICE_SESSION_TERMINAL_UNTIL_KEY = "__p4TerminalUntilMs";
const DEVICE_SESSION_TRANSITION_REVISION_KEY = "__p4TransitionRevision";
const DEVICE_SESSION_SNAPSHOT_MISSING_SINCE_KEY = "__p4SnapshotMissingSinceMs";
const MAX_DEVICE_SESSION_TRANSITION_REVISION = Number.MAX_SAFE_INTEGER;
const ACTIVE_DEVICE_SESSION_STATE_SET = new Set(ACTIVE_DEVICE_SESSION_STATES);
const TERMINAL_DEVICE_SESSION_STATE_SET = new Set(TERMINAL_DEVICE_SESSION_STATES);
const IMPLICIT_TERMINAL_DEVICE_SESSION_STATE_SET = new Set(["idle", "sleeping"]);

const normalizedSessionState = (session) => typeof session?.state === "string"
  ? session.state.trim().toLowerCase()
  : "";

const normalizedSessionText = (value) => typeof value === "string"
  ? value.replace(/\s+/g, " ").trim()
  : "";

export function normalizeSessionDisplayEnabled(value) {
  if (value === true || value === "true") return true;
  if (value === false || value === "false") return false;
  return DEFAULT_SESSION_DISPLAY_ENABLED;
}

export function isActiveDeviceSession(session) {
  return ACTIVE_DEVICE_SESSION_STATE_SET.has(normalizedSessionState(session));
}

export function isTerminalDeviceSession(session) {
  const state = normalizedSessionState(session);
  return TERMINAL_DEVICE_SESSION_STATE_SET.has(state)
    || ["complete", "completed", "failed"].includes(state);
}

export function filterActiveDeviceSessions(sessions) {
  return (Array.isArray(sessions) ? sessions : []).filter(isActiveDeviceSession);
}

function canonicalTerminalState(session) {
  return ["error", "failed"].includes(normalizedSessionState(session)) ? "error" : "done";
}

function isImplicitTerminalDeviceSession(session) {
  return IMPLICIT_TERMINAL_DEVICE_SESSION_STATE_SET.has(normalizedSessionState(session));
}

function normalizedSessionId(session) {
  return typeof session?.id === "string" ? session.id.trim() : "";
}

function deviceSessionVisualIdentity(session) {
  const title = [session?.name, session?.displayTitle, session?.summary]
    .map(normalizedSessionText)
    .find((value) => value && !value.startsWith("<recommended_plugins>")) || "";
  const summary = normalizedSessionText(session?.summary);
  const content = normalizedSessionText(session?.displayContent)
    || (summary && summary !== title && !summary.startsWith("<recommended_plugins>")
      ? summary
      : "");
  if (!title || !content) return "";
  return JSON.stringify([
    normalizedSessionText(session?.transcriptPath),
    normalizedSessionText(session?.cwd),
    title,
    content,
  ]);
}

function deviceSessionSourceFreshness(session) {
  return Math.max(
    Number(session?.statusUpdatedAt || 0),
    Number(session?.lastModified || 0),
  );
}

function dedupeVisuallyIdenticalSessions(sessions) {
  const output = [];
  const identityIndexes = new Map();
  for (const session of sessions) {
    const identity = deviceSessionVisualIdentity(session);
    if (!identity || !identityIndexes.has(identity)) {
      if (identity) identityIndexes.set(identity, output.length);
      output.push(session);
      continue;
    }
    const index = identityIndexes.get(identity);
    const existing = output[index];
    const incomingFreshness = deviceSessionSourceFreshness(session);
    const existingFreshness = deviceSessionSourceFreshness(existing);
    const incomingTransition = deviceSessionTransitionRevision(session);
    const existingTransition = deviceSessionTransitionRevision(existing);
    if (
      incomingFreshness > existingFreshness
      || (incomingFreshness === existingFreshness && incomingTransition > existingTransition)
      || (incomingFreshness === existingFreshness
        && incomingTransition === existingTransition
        && isTerminalDeviceSession(session)
        && !isTerminalDeviceSession(existing))
    ) {
      output[index] = session;
    }
  }
  return output;
}

function normalizedTransitionRevision(value) {
  const revision = Number(value);
  if (!Number.isFinite(revision) || revision <= 0) return 0;
  return Math.min(MAX_DEVICE_SESSION_TRANSITION_REVISION, Math.floor(revision));
}

function transitionState(session) {
  return isTerminalDeviceSession(session)
    ? canonicalTerminalState(session)
    : normalizedSessionState(session);
}

export function deviceSessionTransitionRevision(session) {
  return normalizedTransitionRevision(
    session?.[DEVICE_SESSION_TRANSITION_REVISION_KEY] ?? session?.transitionRevision,
  );
}

export function deviceSessionTerminalUntilMs(session) {
  if (!isTerminalDeviceSession(session)) return 0;
  const value = Number(session?.[DEVICE_SESSION_TERMINAL_UNTIL_KEY] || 0);
  return Number.isFinite(value) && value > 0 ? Math.floor(value) : 0;
}

export function deviceSessionTerminalRemainingMs(session, nowMs = Date.now()) {
  const terminalUntil = deviceSessionTerminalUntilMs(session);
  const now = Number(nowMs);
  if (!terminalUntil || !Number.isFinite(now)) return 0;
  return Math.min(
    DEVICE_SESSION_TERMINAL_HOLD_MS,
    Math.max(0, Math.floor(terminalUntil - now)),
  );
}

function deviceSessionSnapshotMissingSinceMs(session) {
  const value = Number(session?.[DEVICE_SESSION_SNAPSHOT_MISSING_SINCE_KEY] || 0);
  return Number.isFinite(value) && value > 0 ? Math.floor(value) : 0;
}

function nextTransitionRevision(previous, current, currentTime) {
  const previousRevision = deviceSessionTransitionRevision(previous);
  const incomingRevision = normalizedTransitionRevision(current?.transitionRevision);
  const statusRevision = normalizedTransitionRevision(current?.statusUpdatedAt);
  const timeRevision = normalizedTransitionRevision(currentTime);
  return Math.min(
    MAX_DEVICE_SESSION_TRANSITION_REVISION,
    Math.max(previousRevision + 1, incomingRevision, statusRevision, timeRevision, 1),
  );
}

function withTransitionMetadata(previous, current, currentTime, terminalUntil = 0) {
  const previousRevision = deviceSessionTransitionRevision(previous);
  const suppliedRevision = normalizedTransitionRevision(current?.transitionRevision);
  const sameTransition = previous
    && transitionState(previous) === transitionState(current);
  const revision = sameTransition && previousRevision > 0
    ? Math.max(previousRevision, suppliedRevision)
    : nextTransitionRevision(previous, current, currentTime);
  return {
    ...current,
    [DEVICE_SESSION_TRANSITION_REVISION_KEY]: revision,
    [DEVICE_SESSION_TERMINAL_UNTIL_KEY]: terminalUntil,
  };
}

export function reconcileDeviceSessionQueue(
  previousSessions,
  incomingSessions,
  nowMs = Date.now(),
  limit = 8,
  { authoritativeSnapshot = false } = {},
) {
  const now = Number(nowMs);
  const currentTime = Number.isFinite(now) ? now : Date.now();
  const queueLimit = Math.max(0, Math.floor(Number(limit) || 0));
  if (queueLimit === 0) return [];

  const incoming = Array.isArray(incomingSessions) ? incomingSessions : [];
  const incomingById = new Map();
  for (const session of incoming) {
    const id = normalizedSessionId(session);
    if (id && !incomingById.has(id)) incomingById.set(id, session);
  }

  const candidates = [];
  const addedIds = new Set();
  const append = (session) => {
    const id = normalizedSessionId(session);
    if (!id || addedIds.has(id)) return;
    addedIds.add(id);
    candidates.push(session);
  };

  for (const previous of Array.isArray(previousSessions) ? previousSessions : []) {
    const id = normalizedSessionId(previous);
    if (!id) continue;
    const current = incomingById.get(id);
    if (isActiveDeviceSession(current)) {
      append(withTransitionMetadata(previous, current, currentTime));
      continue;
    }

    if (isActiveDeviceSession(previous) && isTerminalDeviceSession(current)) {
      append(withTransitionMetadata(
        previous,
        {
          ...current,
          state: canonicalTerminalState(current),
        },
        currentTime,
        currentTime + DEVICE_SESSION_TERMINAL_HOLD_MS,
      ));
      continue;
    }

    // Claude Desktop can close a turn with only an idle snapshot instead of a
    // durable done event. It is still a real terminal transition when the card
    // was already visible as active, so retain it exactly like explicit done.
    if (isActiveDeviceSession(previous) && isImplicitTerminalDeviceSession(current)) {
      append(withTransitionMetadata(
        previous,
        {
          ...previous,
          ...current,
          state: "done",
        },
        currentTime,
        currentTime + DEVICE_SESSION_TERMINAL_HOLD_MS,
      ));
      continue;
    }

    // Ordered events omit unrelated live Sessions. Full scans can also miss a
    // freshly-created transcript for a few passes, so require continuous
    // absence before removing an otherwise active card.
    if (isActiveDeviceSession(previous) && !current) {
      if (!authoritativeSnapshot) {
        append(previous);
        continue;
      }
      const missingSince = deviceSessionSnapshotMissingSinceMs(previous) || currentTime;
      if (currentTime - missingSince < DEVICE_SESSION_SNAPSHOT_MISSING_GRACE_MS) {
        append({
          ...previous,
          [DEVICE_SESSION_SNAPSHOT_MISSING_SINCE_KEY]: missingSince,
        });
      } else {
        // Some Agent surfaces remove a completed Session from their snapshot
        // without ever exposing done/error. Count the hold from its first
        // absence so a transient scan miss does not restart or extend the TTL.
        const terminalUntil = missingSince + DEVICE_SESSION_TERMINAL_HOLD_MS;
        if (terminalUntil > currentTime) {
          append(withTransitionMetadata(
            previous,
            {
              ...previous,
              state: "done",
            },
            currentTime,
            terminalUntil,
          ));
        }
      }
      continue;
    }

    const terminalUntil = Number(previous?.[DEVICE_SESSION_TERMINAL_UNTIL_KEY] || 0);
    if (isTerminalDeviceSession(previous) && terminalUntil > currentTime) {
      const source = isTerminalDeviceSession(current) ? current : previous;
      append(withTransitionMetadata(
        previous,
        {
          ...source,
          state: canonicalTerminalState(source),
        },
        currentTime,
        terminalUntil,
      ));
    }
  }

  for (const session of incoming) {
    if (isActiveDeviceSession(session)) {
      append(withTransitionMetadata(null, session, currentTime));
    }
  }

  const dedupedCandidates = dedupeVisuallyIdenticalSessions(candidates);
  if (dedupedCandidates.length <= queueLimit) return dedupedCandidates;
  const selectedIds = new Set(
    dedupedCandidates
      .filter(isActiveDeviceSession)
      .slice(0, queueLimit)
      .map(normalizedSessionId),
  );
  for (const session of dedupedCandidates) {
    if (selectedIds.size >= queueLimit) break;
    selectedIds.add(normalizedSessionId(session));
  }
  return dedupedCandidates.filter((session) => selectedIds.has(normalizedSessionId(session)));
}

function loadStoredValue(storage, key, legacyKey) {
  const value = storage?.getItem(key);
  return value == null ? storage?.getItem(legacyKey) : value;
}

export function loadSessionDisplayEnabled(storage = globalThis.localStorage) {
  try {
    return normalizeSessionDisplayEnabled(
      loadStoredValue(
        storage,
        SESSION_DISPLAY_ENABLED_STORAGE_KEY,
        LEGACY_CODEX_SESSION_DISPLAY_ENABLED_STORAGE_KEY,
      ),
    );
  } catch {
    return DEFAULT_SESSION_DISPLAY_ENABLED;
  }
}

export function saveSessionDisplayEnabled(value, storage = globalThis.localStorage) {
  const normalizedValue = normalizeSessionDisplayEnabled(value);
  try {
    storage?.setItem(SESSION_DISPLAY_ENABLED_STORAGE_KEY, String(normalizedValue));
  } catch {}
  return normalizedValue;
}
