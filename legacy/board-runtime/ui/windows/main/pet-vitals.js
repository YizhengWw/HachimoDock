// Helpers for persistent pet growth metrics derived from runtime bridge frames.
(function attachPetClawPetVitals(global) {
    const DEFAULT_STORAGE_KEY = "petClaw.petVitals.v1";
    const SESSION_TTL_MS = 30 * 24 * 60 * 60 * 1000;
    const MAX_SESSION_ENTRIES = 256;

    function readFiniteNumber(...values) {
        for (const value of values) {
            const numeric = Number(value);
            if (Number.isFinite(numeric)) {
                return numeric;
            }
        }
        return undefined;
    }

    function normalizeText(value, fallback = "") {
        return String(value || "").trim() || fallback;
    }

    function clamp(value, min, max) {
        return Math.min(max, Math.max(min, value));
    }

    function normalizeSessionMap(rawSessions = {}) {
        const sessions = {};
        if (!rawSessions || typeof rawSessions !== "object") {
            return sessions;
        }

        for (const [key, value] of Object.entries(rawSessions)) {
            const sessionKey = normalizeText(key);
            if (!sessionKey || !value || typeof value !== "object") {
                continue;
            }

            const lastTotalTokens = readFiniteNumber(value.lastTotalTokens);
            const updatedAt = readFiniteNumber(value.updatedAt, 0);
            if (!Number.isFinite(lastTotalTokens) || lastTotalTokens < 0) {
                continue;
            }

            sessions[sessionKey] = {
                lastTotalTokens,
                updatedAt: Number.isFinite(updatedAt) ? Math.max(0, updatedAt) : 0,
            };
        }

        return sessions;
    }

    function normalizeTrackerState(rawState = {}) {
        const lifetimeExp = readFiniteNumber(rawState.lifetimeExp, 0);
        return {
            lifetimeExp: Number.isFinite(lifetimeExp) ? Math.max(0, Math.floor(lifetimeExp)) : 0,
            sessions: normalizeSessionMap(rawState.sessions),
        };
    }

    function cloneState(state) {
        return {
            lifetimeExp: Number(state?.lifetimeExp || 0),
            sessions: { ...(state?.sessions || {}) },
        };
    }

    function readStorageValue(storage, storageKey) {
        if (!storage || typeof storage.getItem !== "function") {
            return null;
        }

        try {
            return storage.getItem(storageKey);
        } catch {
            return null;
        }
    }

    function writeStorageValue(storage, storageKey, value) {
        if (!storage || typeof storage.setItem !== "function") {
            return false;
        }

        try {
            storage.setItem(storageKey, value);
            return true;
        } catch {
            return false;
        }
    }

    function loadTrackerState(storage, storageKey = DEFAULT_STORAGE_KEY) {
        const raw = readStorageValue(storage, storageKey);
        if (!raw) {
            return normalizeTrackerState();
        }

        try {
            return normalizeTrackerState(JSON.parse(raw));
        } catch {
            return normalizeTrackerState();
        }
    }

    function persistTrackerState(storage, state, storageKey = DEFAULT_STORAGE_KEY) {
        return writeStorageValue(storage, storageKey, JSON.stringify(normalizeTrackerState(state)));
    }

    function buildSessionKey(payload = {}) {
        const source = normalizeText(payload.source, "unknown");
        const sessionId = normalizeText(payload.sessionId);
        const runId = normalizeText(payload.runId);
        const sessionKey = normalizeText(payload.sessionKey);

        if (sessionId) return `${source}:session:${sessionId}`;
        if (runId) return `${source}:run:${runId}`;
        if (sessionKey) return `${source}:key:${sessionKey}`;
        return `${source}:source`;
    }

    function pruneSessions(sessions = {}, nowMs = Date.now()) {
        const nextEntries = Object.entries(sessions)
            .filter(([, value]) => {
                const updatedAt = readFiniteNumber(value?.updatedAt, 0);
                if (!Number.isFinite(updatedAt) || updatedAt <= 0) {
                    return true;
                }
                return nowMs - updatedAt <= SESSION_TTL_MS;
            })
            .sort((left, right) => {
                const leftUpdatedAt = readFiniteNumber(left[1]?.updatedAt, 0) || 0;
                const rightUpdatedAt = readFiniteNumber(right[1]?.updatedAt, 0) || 0;
                return rightUpdatedAt - leftUpdatedAt;
            })
            .slice(0, MAX_SESSION_ENTRIES);

        return Object.fromEntries(nextEntries);
    }

    function applyPayloadToState(previousState, payload, nowMs = Date.now()) {
        const nextState = cloneState(normalizeTrackerState(previousState));
        if (!payload || typeof payload !== "object") {
            nextState.sessions = pruneSessions(nextState.sessions, nowMs);
            return nextState;
        }

        const totalTokens = readFiniteNumber(payload?.tokenUsage?.totalTokens);
        if (!Number.isFinite(totalTokens) || totalTokens < 0) {
            nextState.sessions = pruneSessions(nextState.sessions, nowMs);
            return nextState;
        }

        const sessionKey = buildSessionKey(payload);
        const previousSession = nextState.sessions[sessionKey] || {
            lastTotalTokens: 0,
            updatedAt: 0,
        };
        const previousTotal = readFiniteNumber(previousSession.lastTotalTokens, 0) || 0;

        if (totalTokens >= previousTotal) {
            const delta = Math.max(0, totalTokens - previousTotal);
            if (delta > 0) {
                nextState.lifetimeExp += delta;
            }
        }

        nextState.sessions[sessionKey] = {
            lastTotalTokens: totalTokens,
            updatedAt: Math.max(0, Math.floor(readFiniteNumber(nowMs, Date.now()) || Date.now())),
        };
        nextState.sessions = pruneSessions(nextState.sessions, nowMs);
        return normalizeTrackerState(nextState);
    }

    function computeLevelProgress(lifetimeExp = 0) {
        const exp = Math.max(0, Math.floor(readFiniteNumber(lifetimeExp, 0) || 0));
        const level = Math.floor(Math.sqrt(exp / 1000)) + 1;
        const levelStartExp = Math.pow(level - 1, 2) * 1000;
        const levelEndExp = Math.pow(level, 2) * 1000;
        const span = Math.max(1, levelEndExp - levelStartExp);
        const expPercent = clamp(((exp - levelStartExp) / span) * 100, 0, 100);

        return {
            lifetimeExp: exp,
            level,
            levelStartExp,
            levelEndExp,
            expPercent,
        };
    }

    function summarizeRuntimeTokens(sourceStats = []) {
        return (Array.isArray(sourceStats) ? sourceStats : []).reduce((sum, item) => {
            const totalTokens = readFiniteNumber(item?.totalTokens, 0) || 0;
            return sum + Math.max(0, totalTokens);
        }, 0);
    }

    function computePetVitals(payload, sourceStats, options = {}) {
        const metrics = payload?.metrics || {};
        const tokenUsage = payload?.tokenUsage || {};
        const runtimeTokens = summarizeRuntimeTokens(sourceStats);
        const levelProgress = computeLevelProgress(options.lifetimeExp);

        const brainLoad = readFiniteNumber(metrics.contextUsagePct, 0) || 0;
        let brainStatus = "清醒";
        if (brainLoad > 80) brainStatus = "极度疲劳";
        else if (brainLoad > 50) brainStatus = "略显困倦";

        const cacheHit = tokenUsage.inputTokens > 0
            ? (tokenUsage.cachedInputTokens / tokenUsage.inputTokens * 100).toFixed(1)
            : 0;

        const moodFrustration = (readFiniteNumber(metrics.toolErrors, 0) || 0) * 20;

        const latency = readFiniteNumber(metrics?.latency?.firstTokenMs, 0) || 0;
        let focusLevel = "专注";
        if (latency > 2000) focusLevel = "走神中";
        else if (latency > 0 && latency < 800) focusLevel = "极度兴奋";

        return {
            level: levelProgress.level,
            expPercent: levelProgress.expPercent,
            lifetimeExp: levelProgress.lifetimeExp,
            levelStartExp: levelProgress.levelStartExp,
            levelEndExp: levelProgress.levelEndExp,
            runtimeTokens,
            totalTokens: runtimeTokens,
            brainLoad,
            brainStatus,
            cacheHit,
            moodFrustration,
            focusLevel,
            isEating: (tokenUsage.lastInputTokens || 0) > (tokenUsage.lastOutputTokens || 0),
        };
    }

    function createPetVitalsTracker(options = {}) {
        const storage = options.storage || null;
        const storageKey = normalizeText(options.storageKey, DEFAULT_STORAGE_KEY);
        let state = loadTrackerState(storage, storageKey);

        return {
            applyPayload(payload, nowMs = Date.now()) {
                state = applyPayloadToState(state, payload, nowMs);
                persistTrackerState(storage, state, storageKey);
                return cloneState(state);
            },
            getState() {
                return cloneState(state);
            },
            getLifetimeExp() {
                return Math.max(0, Number(state?.lifetimeExp || 0));
            },
            reset() {
                state = normalizeTrackerState();
                persistTrackerState(storage, state, storageKey);
                return cloneState(state);
            },
        };
    }

    const exported = {
        DEFAULT_STORAGE_KEY,
        applyPayloadToState,
        buildSessionKey,
        computeLevelProgress,
        computePetVitals,
        createPetVitalsTracker,
        loadTrackerState,
        normalizeTrackerState,
    };

    global.PET_CLAW_PET_VITALS = exported;

    if (typeof module !== "undefined" && module.exports) {
        module.exports = exported;
    }
})(typeof window !== "undefined" ? window : globalThis);
