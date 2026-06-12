// Helpers for presenting multi-agent runtime state in the detail panel.
(function attachPetClawDetailRuntimeStats(global) {
    const CLI_SOURCE_PATTERNS = Object.freeze([
        "claude",
        "codex",
        "copilot",
        "gemini",
        "cursor",
        "codebuddy"
    ]);

    function normalizeText(value, fallback = "") {
        return String(value || "").trim() || fallback;
    }

    function cloneValue(value) {
        if (value == null) {
            return value;
        }

        try {
            return JSON.parse(JSON.stringify(value));
        } catch {
            return value;
        }
    }

    function readFiniteNumber(...values) {
        for (const value of values) {
            const numeric = Number(value);
            if (Number.isFinite(numeric)) {
                return numeric;
            }
        }

        return undefined;
    }

    function hasKeys(value) {
        return Boolean(value && typeof value === "object" && Object.keys(value).length > 0);
    }

    function resolveDetailRuntimeSourceLabel(source, fallback = "") {
        const rawSource = normalizeText(source);
        const normalized = rawSource.toLowerCase();

        if (!normalized) {
            return normalizeText(fallback, "桌面活动");
        }

        if (normalized.includes("codex")) return "Codex";
        if (normalized.includes("claude")) return "Claude Code";
        if (normalized.includes("gemini")) return "Gemini";
        if (normalized.includes("cursor")) return "Cursor";
        if (normalized.includes("copilot")) return "Copilot";
        if (normalized.includes("openclaw")) return "OpenClaw";
        if (normalized === "bridge") return "Bridge";

        return rawSource || normalizeText(fallback, "桌面活动");
    }

    function resolveDetailRuntimeChannelLabel(channel, fallback = "") {
        const rawChannel = normalizeText(channel);
        const normalized = rawChannel.toLowerCase();

        if (!normalized) {
            return normalizeText(fallback, "状态流");
        }

        if (normalized === "clawd-hook") return "Hook 状态流";
        if (normalized === "clawd-permission") return "权限请求";
        if (normalized === "codex-log") return "Codex 日志";
        if (normalized === "claude-log") return "Claude 日志";
        if (normalized === "openclaw-gateway") return "OpenClaw 网关";
        if (normalized === "active") return "主状态聚合";

        return rawChannel || normalizeText(fallback, "状态流");
    }

    function buildDetailSourceStats(sourceFrames = []) {
        const groups = new Map();

        for (const entry of Array.isArray(sourceFrames) ? sourceFrames : []) {
            if (!entry || typeof entry !== "object") {
                continue;
            }

            const payload = entry.payload && typeof entry.payload === "object" ? entry.payload : {};
            const source = normalizeText(payload.source, "unknown");
            const channel = normalizeText(payload.channel, "unknown");
            const current = groups.get(source) || {
                source,
                sourceLabel: resolveDetailRuntimeSourceLabel(source, source),
                channels: [],
                channelLabels: [],
                totalTokens: 0,
                toolCalls: 0,
                toolErrors: 0,
                updatedAt: 0,
            };

            current.updatedAt = Math.max(current.updatedAt, Number(entry.updatedAt || 0));

            if (channel && !current.channels.includes(channel)) {
                current.channels.push(channel);
                current.channelLabels.push(resolveDetailRuntimeChannelLabel(channel, channel));
            }

            const totalTokens = Number(payload?.tokenUsage?.totalTokens);
            if (Number.isFinite(totalTokens) && totalTokens > 0) {
                current.totalTokens += totalTokens;
            }

            const toolCalls = Number(payload?.metrics?.toolCalls);
            if (Number.isFinite(toolCalls) && toolCalls >= 0) {
                current.toolCalls += toolCalls;
            }

            const toolErrors = Number(payload?.metrics?.toolErrors);
            if (Number.isFinite(toolErrors) && toolErrors >= 0) {
                current.toolErrors += toolErrors;
            }

            groups.set(source, current);
        }

        return Array.from(groups.values()).sort((left, right) => {
            const updatedDelta = Number(right.updatedAt || 0) - Number(left.updatedAt || 0);
            if (updatedDelta !== 0) {
                return updatedDelta;
            }

            const tokenDelta = Number(right.totalTokens || 0) - Number(left.totalTokens || 0);
            if (tokenDelta !== 0) {
                return tokenDelta;
            }

            return Number(right.toolCalls || 0) - Number(left.toolCalls || 0);
        });
    }

    function mergeDetailRuntimeSourceFrame(previousFrame, incomingFrame) {
        const previous = previousFrame && typeof previousFrame === "object" ? previousFrame : null;
        const incoming = incomingFrame && typeof incomingFrame === "object" ? incomingFrame : null;

        if (!previous) {
            return incoming ? (cloneValue(incoming) || incoming) : null;
        }

        if (!incoming) {
            return cloneValue(previous) || previous;
        }

        const previousPayload = previous.payload && typeof previous.payload === "object"
            ? previous.payload
            : {};
        const incomingPayload = incoming.payload && typeof incoming.payload === "object"
            ? incoming.payload
            : {};
        const mergedPayload = {
            ...(cloneValue(previousPayload) || previousPayload),
            ...(cloneValue(incomingPayload) || incomingPayload),
        };
        const incomingChannel = normalizeText(incomingPayload.channel);
        const previousChannel = normalizeText(previousPayload.channel);
        const keepPreviousTopic = Boolean(incoming.activeTopic) && !Boolean(previous.activeTopic) && normalizeText(previous.topic);

        if (incomingChannel === "active" && previousChannel && previousChannel !== "active") {
            mergedPayload.channel = previousPayload.channel;
        }

        if (!hasKeys(incomingPayload.tokenUsage) && hasKeys(previousPayload.tokenUsage)) {
            mergedPayload.tokenUsage = cloneValue(previousPayload.tokenUsage);
        }

        if (!hasKeys(incomingPayload.metrics) && hasKeys(previousPayload.metrics)) {
            mergedPayload.metrics = cloneValue(previousPayload.metrics);
        }

        return {
            ...(cloneValue(previous) || previous),
            ...(cloneValue(incoming) || incoming),
            source: normalizeText(incoming.source, normalizeText(previous.source, "none")),
            topic: keepPreviousTopic
                ? previous.topic
                : normalizeText(incoming.topic, normalizeText(previous.topic)),
            activeTopic: Boolean(previous.activeTopic || incoming.activeTopic),
            payload: mergedPayload,
            updatedAt: Math.max(
                readFiniteNumber(previous.updatedAt, 0) || 0,
                readFiniteNumber(incoming.updatedAt, 0) || 0
            ),
            observedAt: Math.max(
                readFiniteNumber(previous.observedAt, 0) || 0,
                readFiniteNumber(incoming.observedAt, 0) || 0
            ),
            bridgeAgeMs: readFiniteNumber(incoming.bridgeAgeMs, previous.bridgeAgeMs, null),
            bridgeToWsMs: readFiniteNumber(incoming.bridgeToWsMs, previous.bridgeToWsMs, null),
            endToEndMs: readFiniteNumber(incoming.endToEndMs, previous.endToEndMs, null),
        };
    }

    function summarizeDetailRuntimeChannels(sourceStats = [], options = {}) {
        const items = Array.isArray(sourceStats) ? sourceStats : [];
        const labels = items
            .map((item) => normalizeText(item?.sourceLabel))
            .filter(Boolean);

        if (!labels.length) {
            return "";
        }

        const limit = Number.isFinite(Number(options.limit)) && Number(options.limit) > 0
            ? Math.max(1, Math.floor(Number(options.limit)))
            : 3;
        const visible = labels.slice(0, limit);
        const summary = visible.join("、");

        if (labels.length <= limit) {
            return summary;
        }

        return `${summary} 等 ${labels.length} 个渠道`;
    }

    function isDetailRuntimeCliSource(source = "") {
        const normalized = normalizeText(source).toLowerCase();
        if (!normalized) {
            return false;
        }

        return CLI_SOURCE_PATTERNS.some((pattern) => normalized.includes(pattern));
    }

    function selectDetailRuntimeDisplayStats(sourceStats = []) {
        const items = Array.isArray(sourceStats) ? sourceStats : [];
        const cliItems = items.filter((item) => isDetailRuntimeCliSource(item?.source));
        return cliItems.length > 0 ? cliItems : items;
    }

    const exported = {
        buildDetailSourceStats,
        mergeDetailRuntimeSourceFrame,
        resolveDetailRuntimeSourceLabel,
        resolveDetailRuntimeChannelLabel,
        isDetailRuntimeCliSource,
        selectDetailRuntimeDisplayStats,
        summarizeDetailRuntimeChannels,
    };

    global.PET_CLAW_DETAIL_RUNTIME_STATS = exported;

    if (typeof module !== "undefined" && module.exports) {
        module.exports = exported;
    }
})(typeof window !== "undefined" ? window : globalThis);
