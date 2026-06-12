"use strict";

const fs = require("fs");
const path = require("path");
const defaults = require("./codex");

const MAX_MESSAGE_PREVIEW = 240;
const MAX_DISPLAY_TITLE = 22;

const TEXT_THINKING = "\u6b63\u5728\u601d\u8003";
const TEXT_REPLYING = "\u6b63\u5728\u56de\u590d";
const TEXT_DONE = "\u5df2\u5b8c\u6210";
const TEXT_SESSION_FALLBACK = "Codex \u4f1a\u8bdd";

function readNumber(value) {
  return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

function compactText(value, max = MAX_MESSAGE_PREVIEW) {
  if (typeof value !== "string") return "";
  const normalized = value.replace(/\s+/g, " ").trim();
  if (!normalized) return "";
  return normalized.length > max ? `${normalized.slice(0, Math.max(0, max - 1))}...` : normalized;
}

function wrapDisplayBody(value) {
  return compactText(value, MAX_MESSAGE_PREVIEW);
}

function stripCodexPrefix(sessionId) {
  return typeof sessionId === "string" ? sessionId.replace(/^codex:/, "") : "";
}

function firstLine(value, max = MAX_DISPLAY_TITLE) {
  const text = compactText(value, max * 2);
  if (!text) return "";
  const line = text.split(/[。！？!?\n]/)[0] || text;
  return compactText(line, max);
}

function extractTokenUsage(payload) {
  if (!payload || typeof payload !== "object" || payload.type !== "token_count") return null;
  const info = payload.info && typeof payload.info === "object" ? payload.info : null;
  const total = info && info.total_token_usage && typeof info.total_token_usage === "object"
    ? info.total_token_usage
    : null;
  const last = info && info.last_token_usage && typeof info.last_token_usage === "object"
    ? info.last_token_usage
    : null;

  const usage = {
    inputTokens: readNumber(total && total.input_tokens),
    outputTokens: readNumber(total && total.output_tokens),
    cachedInputTokens: readNumber(total && total.cached_input_tokens),
    reasoningOutputTokens: readNumber(total && total.reasoning_output_tokens),
    totalTokens: readNumber(total && total.total_tokens),
    lastInputTokens: readNumber(last && last.input_tokens),
    lastOutputTokens: readNumber(last && last.output_tokens),
    lastCachedInputTokens: readNumber(last && last.cached_input_tokens),
    lastReasoningOutputTokens: readNumber(last && last.reasoning_output_tokens),
    lastTotalTokens: readNumber(last && last.total_tokens),
    modelContextWindow: readNumber(info && info.model_context_window),
  };
  const hasAny = Object.values(usage).some((value) => Number.isFinite(value));
  return hasAny ? usage : null;
}

class CodexLogMonitor {
  constructor(agentConfig, onState) {
    this.config = { ...defaults, ...(agentConfig || {}) };
    this.onState = typeof onState === "function" ? onState : () => {};
    this.timer = null;
    this.tracked = new Map();
    this.sessionTitles = new Map();
    this.sessionTitleIndexMtime = 0;
  }

  start() {
    if (this.timer) return;
    this.poll();
    this.timer = setInterval(() => this.poll(), this.config.POLL_INTERVAL_MS);
    this.timer.unref?.();
  }

  stop() {
    if (!this.timer) return;
    clearInterval(this.timer);
    this.timer = null;
  }

  refreshSessionTitleIndex() {
    const indexPath = this.config.SESSION_INDEX_PATH;
    if (!indexPath) return;

    let stat;
    try {
      stat = fs.statSync(indexPath);
    } catch {
      return;
    }
    if (stat.mtimeMs === this.sessionTitleIndexMtime) return;

    const next = new Map();
    try {
      const lines = fs.readFileSync(indexPath, "utf8").split("\n");
      for (const rawLine of lines) {
        const line = rawLine.trim();
        if (!line) continue;
        let parsed;
        try {
          parsed = JSON.parse(line);
        } catch {
          continue;
        }
        const id = typeof parsed.id === "string" ? parsed.id : "";
        const title = typeof parsed.thread_name === "string" ? compactText(parsed.thread_name, 120) : "";
        if (id && title) {
          next.set(id, title);
        }
      }
      this.sessionTitles = next;
      this.sessionTitleIndexMtime = stat.mtimeMs;
    } catch {
      // Keep the previous index; title is best-effort.
    }
  }

  getIndexedTitle(sessionId) {
    this.refreshSessionTitleIndex();
    return this.sessionTitles.get(stripCodexPrefix(sessionId)) || "";
  }

  getSessionDirs() {
    const dirs = [];
    const lookback = Math.max(1, Number(this.config.LOOKBACK_DAYS) || 2);
    const now = new Date();

    for (let daysAgo = 0; daysAgo < lookback; daysAgo++) {
      const d = new Date(now);
      d.setDate(d.getDate() - daysAgo);
      const yyyy = d.getFullYear();
      const mm = String(d.getMonth() + 1).padStart(2, "0");
      const dd = String(d.getDate()).padStart(2, "0");
      dirs.push(path.join(this.config.SESSION_DIR, String(yyyy), mm, dd));
    }

    return dirs;
  }

  extractSessionId(fileName) {
    const match = fileName.match(
      /^rollout-.*-([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\.jsonl$/i
    );
    if (match && match[1]) return `codex:${match[1]}`;

    const base = fileName.replace(".jsonl", "");
    const parts = base.split("-");
    if (parts.length < 10) return null;
    return `codex:${parts.slice(-5).join("-")}`;
  }

  ensureTitle(entry) {
    if (entry.sessionTitle) return entry.sessionTitle;
    const indexed = this.getIndexedTitle(entry.sessionId);
    if (indexed) {
      entry.sessionTitle = indexed;
      return indexed;
    }
    const fallback = firstLine(entry.firstUserMessage || entry.lastUserMessage || TEXT_SESSION_FALLBACK, 28);
    entry.sessionTitle = fallback || TEXT_SESSION_FALLBACK;
    return entry.sessionTitle;
  }

  buildDisplay(entry, state, event) {
    const title = compactText(this.ensureTitle(entry), MAX_DISPLAY_TITLE);
    const latestAgentMessage = entry.lastAgentMessage || "";
    const finalAgentMessage = entry.lastTaskCompleteMessage || entry.lastAgentMessage || "";

    let content = "";
    if (event === "event_msg:task_started") {
      content = TEXT_THINKING;
    } else if (event === "event_msg:agent_message") {
      content = latestAgentMessage || TEXT_REPLYING;
    } else if (event === "event_msg:task_complete") {
      content = finalAgentMessage || TEXT_DONE;
    } else if (state === "speaking") {
      content = latestAgentMessage || TEXT_REPLYING;
    } else if (state === "attention") {
      content = finalAgentMessage || TEXT_DONE;
    } else if (state === "thinking") {
      content = TEXT_THINKING;
    } else {
      content = entry.lastDisplayContent || TEXT_THINKING;
    }

    content = wrapDisplayBody(content);
    entry.lastDisplayContent = content;

    return {
      title,
      content,
      status: state,
      event,
      updatedAtMs: Date.now(),
    };
  }

  buildExtra(entry, state, event) {
    const display = this.buildDisplay(entry, state, event);
    return {
      cwd: entry.cwd || "",
      sessionTitle: this.ensureTitle(entry),
      display,
      session: {
        id: entry.sessionId,
        title: this.ensureTitle(entry),
        cwd: entry.cwd || "",
        source: entry.codexSource || "",
        originator: entry.originator || "",
        modelProvider: entry.modelProvider || "",
        cliVersion: entry.cliVersion || "",
        threadSource: entry.threadSource || "",
        firstUserMessage: entry.firstUserMessage || "",
        lastUserMessage: entry.lastUserMessage || "",
        lastAgentMessage: entry.lastAgentMessage || "",
        lastTaskCompleteMessage: entry.lastTaskCompleteMessage || "",
      },
      turn: {
        turnId: entry.turnId || "",
        startedAt: entry.startedAt || undefined,
        completedAt: entry.completedAt || undefined,
        durationMs: entry.durationMs || undefined,
        timeToFirstTokenMs: entry.timeToFirstTokenMs || undefined,
        modelContextWindow: entry.modelContextWindow || undefined,
        collaborationModeKind: entry.collaborationModeKind || "",
      },
      messages: {
        firstUser: entry.firstUserMessage || "",
        lastUser: entry.lastUserMessage || "",
        lastAgent: entry.lastAgentMessage || "",
        lastFinal: entry.lastTaskCompleteMessage || "",
        userMessageCount: entry.userMessageCount || 0,
        agentMessageCount: entry.agentMessageCount || 0,
      },
      tokenUsage: entry.tokenUsage || undefined,
    };
  }

  emit(entry, state, event) {
    const extra = this.buildExtra(entry, state, event);
    const fingerprint = JSON.stringify({
      state,
      event,
      display: extra.display,
      final: extra.messages.lastFinal,
      tokenUsage: extra.tokenUsage && {
        totalTokens: extra.tokenUsage.totalTokens,
        inputTokens: extra.tokenUsage.inputTokens,
        outputTokens: extra.tokenUsage.outputTokens,
        cachedInputTokens: extra.tokenUsage.cachedInputTokens,
        reasoningOutputTokens: extra.tokenUsage.reasoningOutputTokens,
      },
    });
    if (entry.lastEmitFingerprint === fingerprint) return;
    entry.lastEmitFingerprint = fingerprint;

    try {
      this.onState(entry.sessionId, state, event, extra);
    } catch {}
  }

  processLine(line, entry) {
    let obj;
    try {
      obj = JSON.parse(line);
    } catch {
      return;
    }

    const type = obj && typeof obj.type === "string" ? obj.type : "";
    const payload = obj && typeof obj.payload === "object" ? obj.payload : null;
    if (!type) return;

    const subtype = payload && typeof payload.type === "string" ? payload.type : "";
    const key = subtype ? `${type}:${subtype}` : type;

    if (type === "session_meta" && payload) {
      entry.cwd = typeof payload.cwd === "string" ? payload.cwd : "";
      entry.originator = typeof payload.originator === "string" ? payload.originator : "";
      entry.cliVersion = typeof payload.cli_version === "string" ? payload.cli_version : "";
      entry.codexSource = typeof payload.source === "string" ? payload.source : "";
      entry.modelProvider = typeof payload.model_provider === "string" ? payload.model_provider : "";
      entry.threadSource = typeof payload.thread_source === "string" ? payload.thread_source : "";
      const indexed = this.getIndexedTitle(entry.sessionId);
      if (indexed) entry.sessionTitle = indexed;
    }

    if (key === "event_msg:thread_name_updated" && payload) {
      if (typeof payload.thread_name === "string" && payload.thread_name.trim()) {
        entry.sessionTitle = compactText(payload.thread_name, 120);
      }
    }

    if (key === "event_msg:task_started" && payload) {
      entry.turnId = typeof payload.turn_id === "string" ? payload.turn_id : entry.turnId;
      entry.startedAt = readNumber(payload.started_at) || entry.startedAt;
      entry.modelContextWindow = readNumber(payload.model_context_window) || entry.modelContextWindow;
      entry.collaborationModeKind = typeof payload.collaboration_mode_kind === "string"
        ? payload.collaboration_mode_kind
        : entry.collaborationModeKind;
    }

    if (key === "event_msg:user_message" && payload) {
      const message = compactText(payload.message || "", MAX_MESSAGE_PREVIEW);
      if (message) {
        entry.lastUserMessage = message;
        if (!entry.firstUserMessage) entry.firstUserMessage = message;
        if (!entry.sessionTitle) {
          const indexed = this.getIndexedTitle(entry.sessionId);
          entry.sessionTitle = indexed || firstLine(message, 28);
        }
      }
      entry.userMessageCount = (entry.userMessageCount || 0) + 1;
    }

    if (key === "event_msg:agent_message" && payload) {
      const message = compactText(payload.message || "", MAX_MESSAGE_PREVIEW);
      if (message) entry.lastAgentMessage = message;
      entry.agentMessageCount = (entry.agentMessageCount || 0) + 1;
      entry.agentPhase = typeof payload.phase === "string" ? payload.phase : "";
    }

    if (key === "event_msg:task_complete" && payload) {
      const message = compactText(payload.last_agent_message || "", MAX_MESSAGE_PREVIEW);
      if (message) {
        entry.lastTaskCompleteMessage = message;
        entry.lastAgentMessage = message;
      }
      entry.turnId = typeof payload.turn_id === "string" ? payload.turn_id : entry.turnId;
      entry.completedAt = readNumber(payload.completed_at) || entry.completedAt;
      entry.durationMs = readNumber(payload.duration_ms) || entry.durationMs;
      entry.timeToFirstTokenMs = readNumber(payload.time_to_first_token_ms) || entry.timeToFirstTokenMs;
    }

    if (key === "event_msg:token_count") {
      const usage = extractTokenUsage(payload);
      if (!usage) return;
      entry.tokenUsage = usage;
      entry.lastEventTime = Date.now();
      this.emit(entry, entry.lastState || "speaking", key);
      return;
    }

    const state = this.config.LOG_EVENT_MAP[key];
    if (typeof state !== "string" || !state) return;

    entry.lastState = state;
    entry.lastEventTime = Date.now();
    this.emit(entry, state, key);
  }

  pollFile(filePath, fileName) {
    let stat;
    try {
      stat = fs.statSync(filePath);
    } catch {
      return;
    }

    let entry = this.tracked.get(filePath);
    if (!entry) {
      const sessionId = this.extractSessionId(fileName);
      if (!sessionId) return;
      const initialTailBytes = Math.max(0, Number(this.config.INITIAL_TAIL_BYTES) || 0);
      const initialOffset = initialTailBytes > 0 && stat.size > initialTailBytes
        ? stat.size - initialTailBytes
        : 0;
      entry = {
        offset: initialOffset,
        partial: "",
        dropFirstLine: initialOffset > 0,
        sessionId,
        cwd: "",
        lastState: null,
        tokenUsage: null,
        sessionTitle: "",
        firstUserMessage: "",
        lastUserMessage: "",
        lastAgentMessage: "",
        lastTaskCompleteMessage: "",
        lastDisplayContent: "",
        userMessageCount: 0,
        agentMessageCount: 0,
        lastEmitFingerprint: "",
        lastEventTime: Date.now(),
      };
      this.tracked.set(filePath, entry);
    }

    if (stat.size <= entry.offset) return;

    let buffer;
    try {
      const fd = fs.openSync(filePath, "r");
      const readLen = stat.size - entry.offset;
      buffer = Buffer.alloc(readLen);
      fs.readSync(fd, buffer, 0, readLen, entry.offset);
      fs.closeSync(fd);
    } catch {
      return;
    }
    entry.offset = stat.size;

    const text = entry.partial + buffer.toString("utf8");
    const lines = text.split("\n");
    entry.partial = lines.pop() || "";
    if (entry.dropFirstLine) {
      lines.shift();
      entry.dropFirstLine = false;
    }

    for (const rawLine of lines) {
      const line = rawLine.trim();
      if (!line) continue;
      this.processLine(line, entry);
    }
  }

  cleanStaleFiles() {
    // Keep file offsets for long-running Codex sessions. Dropping a stale entry
    // makes the next event re-read the whole JSONL; current desktop sessions can
    // be 100MB+, which stalls the bridge and prevents live speech updates.
    for (const [filePath] of this.tracked) {
      if (!fs.existsSync(filePath)) {
        this.tracked.delete(filePath);
      }
    }
  }

  poll() {
    const dirs = this.getSessionDirs();
    const maxAgeMs = Math.max(1000, Number(this.config.NEW_FILE_MAX_AGE_MS) || 120000);
    const now = Date.now();

    for (const dir of dirs) {
      let files = [];
      try {
        files = fs.readdirSync(dir);
      } catch {
        continue;
      }

      for (const file of files) {
        if (!file.startsWith("rollout-") || !file.endsWith(".jsonl")) continue;
        const filePath = path.join(dir, file);

        if (!this.tracked.has(filePath)) {
          try {
            const mtime = fs.statSync(filePath).mtimeMs;
            if (now - mtime > maxAgeMs) continue;
          } catch {
            continue;
          }
        }

        this.pollFile(filePath, file);
      }
    }

    this.cleanStaleFiles();
  }
}

module.exports = CodexLogMonitor;
