"use strict";

/*
 * [Input] Claude Code/Desktop transcript JSONL plus process availability.
 * [Output] Per-session lifecycle cards with restart-safe, deduplicated cumulative token usage, current-day Agent totals, and latest-turn context fields.
 * [Pos] Claude transcript monitor worker used by the managed status bridge.
 * [Sync] If Claude transcript or token aggregation semantics change, update `pc/.folder.md`.
 */

const { execFile } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");
const defaults = require("./claude-code");

const MAX_MESSAGE_PREVIEW = 240;
const MAX_DISPLAY_TITLE = 22;
const DAILY_TOKEN_FIELDS = [
  "totalTokens",
  "inputTokens",
  "outputTokens",
  "cachedInputTokens",
  "cacheCreationInputTokens",
  "reasoningOutputTokens",
];

const TEXT_THINKING = "\u6b63\u5728\u601d\u8003";
const TEXT_REPLYING = "\u6b63\u5728\u56de\u590d";
const TEXT_DONE = "\u5df2\u5b8c\u6210";
const TEXT_SESSION_FALLBACK = "Claude \u4f1a\u8bdd";

function readNumber(value) {
  return typeof value === "number" && Number.isFinite(value) ? value : undefined;
}

function readTimestampMs(value) {
  if (typeof value === "number" && Number.isFinite(value)) {
    return value < 1_000_000_000_000 ? value * 1000 : value;
  }
  if (typeof value !== "string" || !value.trim()) return 0;
  const parsed = Date.parse(value);
  return Number.isFinite(parsed) ? parsed : 0;
}

function localDayKey(value = Date.now()) {
  const date = new Date(value);
  return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, "0")}-${String(date.getDate()).padStart(2, "0")}`;
}

function sumDailyTokenUsage(values) {
  const total = {
    totalTokens: 0,
    inputTokens: 0,
    outputTokens: 0,
    cachedInputTokens: 0,
    cacheCreationInputTokens: 0,
    reasoningOutputTokens: 0,
  };
  for (const usage of values) {
    if (!usage || typeof usage !== "object") continue;
    for (const field of DAILY_TOKEN_FIELDS) {
      if (!Number.isFinite(usage[field])) continue;
      total[field] = (total[field] || 0) + usage[field];
    }
    if (!Number.isFinite(usage.totalTokens)) {
      total.totalTokens += (Number.isFinite(usage.inputTokens) ? usage.inputTokens : 0)
        + (Number.isFinite(usage.outputTokens) ? usage.outputTokens : 0);
    }
  }
  return total;
}

function compactText(value, max = MAX_MESSAGE_PREVIEW) {
  if (typeof value !== "string") return "";
  const normalized = value.replace(/\s+/g, " ").trim();
  if (!normalized) return "";
  return normalized.length > max ? `${normalized.slice(0, Math.max(0, max - 1))}...` : normalized;
}

function firstLine(value, max = MAX_DISPLAY_TITLE) {
  const text = compactText(value, max * 2);
  if (!text) return "";
  const line = text.split(/[。！？!?\n]/)[0] || text;
  return compactText(line, max);
}

function contentToText(content) {
  if (typeof content === "string") return compactText(content);
  if (!Array.isArray(content)) return "";

  const parts = [];
  for (const item of content) {
    if (!item || typeof item !== "object") continue;
    if (typeof item.text === "string") {
      parts.push(item.text);
    } else if (typeof item.content === "string") {
      parts.push(item.content);
    }
  }
  return compactText(parts.join(" "));
}

function extractMessageText(obj) {
  if (!obj || typeof obj !== "object") return "";
  if (typeof obj.content === "string" || Array.isArray(obj.content)) {
    return contentToText(obj.content);
  }
  const message = obj.message && typeof obj.message === "object" ? obj.message : null;
  if (message) {
    const fromMessage = contentToText(message.content);
    if (fromMessage) return fromMessage;
  }
  if (typeof obj.text === "string") return compactText(obj.text);
  if (typeof obj.summary === "string") return compactText(obj.summary);
  return "";
}

// An assistant turn that calls a tool is NOT finished — the agent keeps working
// (more tool calls / a final answer follow). Detect tool_use either from the
// message content blocks or the stop_reason so a mid-turn step is not mistaken
// for task completion.
function messageHasToolUse(obj) {
  if (!obj || typeof obj !== "object") return false;
  const message = obj.message && typeof obj.message === "object" ? obj.message : obj;
  if (message && message.stop_reason === "tool_use") return true;
  const content = Array.isArray(message && message.content)
    ? message.content
    : Array.isArray(obj.content)
      ? obj.content
      : null;
  if (!content) return false;
  for (const item of content) {
    if (item && typeof item === "object" && item.type === "tool_use") return true;
  }
  return false;
}

function extractSessionId(obj, fallback) {
  if (obj && typeof obj.session_id === "string" && obj.session_id) return obj.session_id;
  if (obj && typeof obj.sessionId === "string" && obj.sessionId) return obj.sessionId;
  return fallback || "";
}

function extractTokenUsage(obj) {
  const candidates = [
    obj && obj.usage,
    obj && obj.message && obj.message.usage,
    obj && obj.tokenUsage,
    obj && obj.token_usage,
  ].filter((value) => value && typeof value === "object");
  const usage = candidates[0];
  if (!usage) return null;

  const inputTokens = readNumber(usage.input_tokens) ?? readNumber(usage.inputTokens);
  const outputTokens = readNumber(usage.output_tokens) ?? readNumber(usage.outputTokens);
  const cacheCreationInputTokens = readNumber(usage.cache_creation_input_tokens) ?? readNumber(usage.cacheCreationInputTokens);
  const cachedInputTokens = readNumber(usage.cache_read_input_tokens) ?? readNumber(usage.cached_input_tokens) ?? readNumber(usage.cachedInputTokens);
  const reasoningOutputTokens = readNumber(usage.reasoning_output_tokens) ?? readNumber(usage.reasoningOutputTokens);
  const totalTokens = readNumber(usage.total_tokens) ?? readNumber(usage.totalTokens);
  const parts = [inputTokens, outputTokens, cachedInputTokens, cacheCreationInputTokens].filter(Number.isFinite);
  const normalized = {
    inputTokens,
    outputTokens,
    cachedInputTokens,
    cacheCreationInputTokens,
    reasoningOutputTokens,
    totalTokens: totalTokens ?? (parts.length ? parts.reduce((sum, value) => sum + value, 0) : undefined),
    modelContextWindow: readNumber(usage.model_context_window) ?? readNumber(usage.modelContextWindow),
  };
  return Object.values(normalized).some(Number.isFinite) ? normalized : null;
}

function tokenUsageEventKey(obj) {
  const candidates = [
    ["message", obj?.message?.id],
    ["request", obj?.requestId],
    ["entry", obj?.uuid],
  ];
  for (const [kind, value] of candidates) {
    if (typeof value === "string" && value.trim()) return `${kind}:${value.trim()}`;
  }
  return "";
}

function accumulateTokenUsage(
  entry,
  usage,
  obj,
  usageField = "tokenUsage",
  eventIdsField = "tokenUsageEventIds",
) {
  if (!entry || !usage) return false;
  const eventKey = tokenUsageEventKey(obj);
  if (eventKey && entry[eventIdsField].has(eventKey)) return false;
  if (eventKey) entry[eventIdsField].add(eventKey);

  const previous = entry[usageField] || {};
  const next = { ...previous };
  for (const field of [
    "inputTokens",
    "outputTokens",
    "cachedInputTokens",
    "cacheCreationInputTokens",
    "reasoningOutputTokens",
    "totalTokens",
  ]) {
    if (!Number.isFinite(usage[field])) continue;
    next[field] = (Number.isFinite(previous[field]) ? previous[field] : 0) + usage[field];
  }
  for (const [lastField, usageField] of [
    ["lastInputTokens", "inputTokens"],
    ["lastOutputTokens", "outputTokens"],
    ["lastCachedInputTokens", "cachedInputTokens"],
    ["lastCacheCreationInputTokens", "cacheCreationInputTokens"],
    ["lastReasoningOutputTokens", "reasoningOutputTokens"],
    ["lastTotalTokens", "totalTokens"],
  ]) {
    if (Number.isFinite(usage[usageField])) next[lastField] = usage[usageField];
  }
  if (Number.isFinite(usage.modelContextWindow)) {
    next.modelContextWindow = usage.modelContextWindow;
  }
  entry[usageField] = next;
  return true;
}

function claudeProjectsRoot(env = process.env) {
  const home = env.CLAUDE_HOME || env.HOME || env.USERPROFILE || os.homedir();
  return path.join(home, ".claude", "projects");
}

class ClaudeLogMonitor {
  constructor(agentConfig, onState) {
    this.config = { ...defaults, ...(agentConfig || {}) };
    this.onState = typeof onState === "function" ? onState : () => {};
    this.timer = null;
    this.lastRunning = null;
    this.lastEmitMs = 0;
    this.tracked = new Map();
    this.processProbeAt = 0;
    this.processProbeInFlight = false;
    this.dailyUsageDayKey = localDayKey();
    this.dailyUsageBySession = new Map();
  }

  start() {
    if (this.timer) return;
    this.baselineExistingSessions();
    this.pollProcessState();
    this.timer = setInterval(() => this.poll(), this.config.POLL_INTERVAL_MS);
    this.timer.unref?.();
  }

  stop() {
    if (!this.timer) return;
    clearInterval(this.timer);
    this.timer = null;
  }

  emitProcess(state, event) {
    try {
      this.onState(this.config.DEFAULT_SESSION_ID, state, event, {});
    } catch {}
  }

  emit(entry, state, event) {
    const extra = this.buildExtra(entry, state, event);
    const fingerprint = JSON.stringify({
      state,
      event,
      display: extra.display,
      tokenUsage: extra.tokenUsage && {
        totalTokens: extra.tokenUsage.totalTokens,
        inputTokens: extra.tokenUsage.inputTokens,
        outputTokens: extra.tokenUsage.outputTokens,
        cachedInputTokens: extra.tokenUsage.cachedInputTokens,
        cacheCreationInputTokens: extra.tokenUsage.cacheCreationInputTokens,
      },
      dailyTokenUsage: extra.dailyTokenUsage,
    });
    if (entry.lastEmitFingerprint === fingerprint) return;
    entry.lastEmitFingerprint = fingerprint;

    try {
      this.onState(entry.sessionId, state, event, extra);
    } catch {}
  }

  ensureTitle(entry) {
    if (entry.sessionTitle) return entry.sessionTitle;
    const fallback = firstLine(entry.firstUserMessage || entry.lastUserMessage || TEXT_SESSION_FALLBACK, 28);
    entry.sessionTitle = fallback || TEXT_SESSION_FALLBACK;
    return entry.sessionTitle;
  }

  buildDisplay(entry, state, event) {
    const title = compactText(this.ensureTitle(entry), MAX_DISPLAY_TITLE);
    let content = "";
    if (event === "claude:user_message") {
      content = TEXT_THINKING;
    } else if (event === "claude:assistant_message") {
      content = entry.lastAgentMessage || TEXT_DONE;
    } else if (state === "speaking") {
      content = entry.lastAgentMessage || TEXT_REPLYING;
    } else if (state === "done") {
      content = entry.lastAgentMessage || TEXT_DONE;
    } else {
      content = entry.lastDisplayContent || TEXT_THINKING;
    }

    content = compactText(content, MAX_MESSAGE_PREVIEW);
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
    const dailyTokenUsage = this.buildDailyTokenUsage();
    return {
      cwd: entry.cwd || "",
      sessionTitle: this.ensureTitle(entry),
      display,
      session: {
        id: entry.sessionId,
        title: this.ensureTitle(entry),
        cwd: entry.cwd || "",
        firstUserMessage: entry.firstUserMessage || "",
        lastUserMessage: entry.lastUserMessage || "",
        lastAgentMessage: entry.lastAgentMessage || "",
      },
      messages: {
        firstUser: entry.firstUserMessage || "",
        lastUser: entry.lastUserMessage || "",
        lastAgent: entry.lastAgentMessage || "",
        userMessageCount: entry.userMessageCount || 0,
        agentMessageCount: entry.agentMessageCount || 0,
      },
      tokenUsage: entry.tokenUsage || undefined,
      dailyTokenUsage,
    };
  }

  accumulateEntryDailyTokenUsage(entry, usage, obj, eventTimestampMs) {
    this.ensureDailyUsageDay();
    if (!entry || !usage || localDayKey(eventTimestampMs) !== this.dailyUsageDayKey) {
      return false;
    }
    if (entry.dailyUsageDayKey !== this.dailyUsageDayKey) {
      entry.dailyUsageDayKey = this.dailyUsageDayKey;
      entry.dailyTokenUsage = null;
      entry.dailyTokenUsageEventIds.clear();
    }
    const changed = accumulateTokenUsage(
      entry,
      usage,
      obj,
      "dailyTokenUsage",
      "dailyTokenUsageEventIds",
    );
    if (changed) this.recordDailyTokenUsage(entry.sessionId, entry.dailyTokenUsage);
    return changed;
  }

  async isRunningOnWindows() {
    const processNames = new Set(
      (Array.isArray(this.config.PROCESS_NAMES_WIN) ? this.config.PROCESS_NAMES_WIN : [])
        .map((name) => String(name).trim().toLowerCase())
        .filter(Boolean)
    );
    if (processNames.size === 0) return false;

    const output = await execFileText("tasklist.exe", ["/FO", "CSV", "/NH"], {
      timeout: 2000,
      windowsHide: true,
    });
    if (output) {
      const lines = output.split(/\r?\n/);
      for (const line of lines) {
        const trimmed = line.trim();
        if (!trimmed) continue;
        const match = trimmed.match(/^"([^"]+)"/);
        if (!match || !match[1]) continue;
        if (processNames.has(match[1].toLowerCase())) return true;
      }
    }

    return false;
  }

  async isRunningOnUnix() {
    const processNames = new Set(
      (Array.isArray(this.config.PROCESS_NAMES_UNIX) ? this.config.PROCESS_NAMES_UNIX : [])
        .map((name) => String(name).trim().toLowerCase())
        .filter(Boolean)
    );
    if (processNames.size === 0) return false;

    const output = await execFileText("ps", ["-A", "-o", "comm="], { timeout: 2000 });
    if (output) {
      const lines = output.split(/\r?\n/);
      for (const line of lines) {
        const command = path.basename(line.trim()).toLowerCase();
        if (!command) continue;
        if (processNames.has(command)) return true;
      }
    }

    return false;
  }

  async isRunning() {
    if (process.platform === "win32") return this.isRunningOnWindows();
    return this.isRunningOnUnix();
  }

  poll() {
    this.pollSessions();
    this.pollProcessState();
  }

  pollProcessState() {
    const now = Date.now();
    const probeIntervalMs = Math.max(
      1000,
      Number(this.config.PROCESS_POLL_INTERVAL_MS) || 30000
    );
    if (this.processProbeInFlight || now - this.processProbeAt < probeIntervalMs) return;

    this.processProbeAt = now;
    this.processProbeInFlight = true;
    Promise.resolve(this.isRunning())
      .then((running) => this.updateRunningState(Boolean(running)))
      .catch(() => this.updateRunningState(false))
      .finally(() => {
        this.processProbeInFlight = false;
      });
  }

  updateRunningState(running) {
    const now = Date.now();
    const heartbeatMs = Math.max(1000, Number(this.config.HEARTBEAT_MS) || 30000);
    const changed = this.lastRunning === null || this.lastRunning !== running;

    this.lastRunning = running;
    // Once a real transcript is known, its session-specific lifecycle must stay
    // authoritative. A process heartbeat uses the synthetic claude:local id and
    // would otherwise become the newest Auto target after every completed turn.
    if (this.tracked.size > 0) return;
    if (!changed && (now - this.lastEmitMs) < heartbeatMs) return;

    this.lastEmitMs = now;
    if (running) this.emitProcess("idle", "process.detected");
    else this.emitProcess("sleeping", "process.missing");
  }

  projectsRoot() {
    if (typeof this.config.PROJECTS_ROOT === "string" && this.config.PROJECTS_ROOT) {
      return this.config.PROJECTS_ROOT;
    }
    return claudeProjectsRoot(this.config.env || process.env);
  }

  listSessionFiles() {
    const root = this.projectsRoot();
    let projects;
    try {
      projects = fs.readdirSync(root, { withFileTypes: true });
    } catch {
      return [];
    }

    const files = [];
    for (const project of projects) {
      if (!project.isDirectory()) continue;
      const projectDir = path.join(root, project.name);
      let entries;
      try {
        entries = fs.readdirSync(projectDir, { withFileTypes: true });
      } catch {
        continue;
      }
      for (const entry of entries) {
        if (!entry.isFile() || !entry.name.endsWith(".jsonl")) continue;
        const filePath = path.join(projectDir, entry.name);
        try {
          const stat = fs.statSync(filePath);
          files.push({
            filePath,
            fileName: entry.name,
            mtimeMs: stat.mtimeMs,
          });
        } catch {}
      }
    }

    files.sort((a, b) => b.mtimeMs - a.mtimeMs);
    const limit = Math.max(1, Number(this.config.MAX_SCAN_FILES) || 200);
    return files.slice(0, limit);
  }

  processLine(line, entry) {
    let obj;
    try {
      obj = JSON.parse(line);
    } catch {
      return;
    }
    if (!obj || typeof obj !== "object") return;

    entry.sessionId = extractSessionId(obj, entry.sessionId);
    if (typeof obj.cwd === "string" && obj.cwd) entry.cwd = obj.cwd;

    const type = typeof obj.type === "string" ? obj.type : "";
    const role = obj.message && typeof obj.message.role === "string" ? obj.message.role : "";
    const usage = extractTokenUsage(obj);
    const tokenUsageChanged = accumulateTokenUsage(entry, usage, obj);
    const eventTimestampMs = readTimestampMs(obj.timestamp) || Date.now();
    this.accumulateEntryDailyTokenUsage(entry, usage, obj, eventTimestampMs);

    if (type === "summary" && typeof obj.summary === "string" && obj.summary.trim()) {
      entry.sessionTitle = compactText(obj.summary, 120);
      return;
    }

    if (type === "user" || role === "user") {
      const message = extractMessageText(obj);
      if (message) {
        entry.lastUserMessage = message;
        if (!entry.firstUserMessage) entry.firstUserMessage = message;
        if (!entry.sessionTitle) entry.sessionTitle = firstLine(message, 28);
      }
      entry.userMessageCount = (entry.userMessageCount || 0) + 1;
      entry.lastState = "working";
      entry.lastEventTime = Date.now();
      this.emit(entry, "working", "claude:user_message");
      return;
    }

    if (type === "assistant" || role === "assistant") {
      const message = extractMessageText(obj);
      if (message) entry.lastAgentMessage = message;
      entry.agentMessageCount = (entry.agentMessageCount || 0) + 1;
      entry.lastEventTime = Date.now();
      // A tool-use step means the turn is still running — stay "working" so the
      // device doesn't fall to idle between an assistant tool call and its result
      // (which can exceed the board's done-hold on a slow tool/step). Only a
      // terminal assistant text (no tool_use) marks the task as done.
      if (messageHasToolUse(obj)) {
        entry.lastState = "working";
        this.emit(entry, "working", "claude:tool_use");
      } else {
        entry.lastState = "done";
        this.emit(entry, "done", "claude:assistant_message");
      }
      return;
    }

    if (tokenUsageChanged) {
      this.emit(entry, entry.lastState || "done", "claude:token_count");
    }
  }

  createEntry(fileName, offset, dropFirstLine) {
    return {
      offset,
      partial: "",
      dropFirstLine,
      sessionId: fileName.replace(/\.jsonl$/, ""),
      cwd: "",
      tokenUsage: null,
      tokenUsageEventIds: new Set(),
      dailyTokenUsage: null,
      dailyTokenUsageEventIds: new Set(),
      dailyUsageDayKey: "",
      sessionTitle: "",
      firstUserMessage: "",
      lastUserMessage: "",
      lastAgentMessage: "",
      lastDisplayContent: "",
      userMessageCount: 0,
      agentMessageCount: 0,
      lastEmitFingerprint: "",
      lastState: null,
      lastEventTime: Date.now(),
    };
  }

  seedEntryTokenUsage(filePath, entry) {
    let contents;
    try {
      contents = fs.readFileSync(filePath, "utf8");
    } catch {
      return;
    }

    for (const rawLine of contents.split("\n")) {
      const line = rawLine.trim();
      if (!line) continue;
      let obj;
      try {
        obj = JSON.parse(line);
      } catch {
        continue;
      }
      if (!obj || typeof obj !== "object") continue;
      entry.sessionId = extractSessionId(obj, entry.sessionId);
      const usage = extractTokenUsage(obj);
      accumulateTokenUsage(entry, usage, obj);
      const eventTimestampMs = readTimestampMs(obj.timestamp) || Date.now();
      this.accumulateEntryDailyTokenUsage(entry, usage, obj, eventTimestampMs);
    }
  }

  ensureDailyUsageDay() {
    const today = localDayKey();
    if (today === this.dailyUsageDayKey) return;
    this.dailyUsageDayKey = today;
    this.dailyUsageBySession.clear();
  }

  recordDailyTokenUsage(sessionId, usage) {
    this.ensureDailyUsageDay();
    if (!sessionId || !usage) return;
    this.dailyUsageBySession.set(sessionId, usage);
  }

  buildDailyTokenUsage() {
    this.ensureDailyUsageDay();
    return sumDailyTokenUsage(this.dailyUsageBySession.values());
  }

  emitDailyTokenSnapshot(entry) {
    if (!entry?.sessionId) return;
    try {
      this.onState(entry.sessionId, "idle", "claude:daily_token_snapshot", {
        tokenUsage: entry.tokenUsage || undefined,
        dailyTokenUsage: this.buildDailyTokenUsage(),
      });
    } catch {}
  }

  trackFileBaseline(filePath, fileName, seedTokenUsage = false) {
    if (this.tracked.has(filePath)) return;
    try {
      const stat = fs.statSync(filePath);
      const entry = this.createEntry(fileName, stat.size, false);
      if (seedTokenUsage) this.seedEntryTokenUsage(filePath, entry);
      this.tracked.set(filePath, entry);
    } catch {}
  }

  baselineExistingSessions() {
    const files = this.listSessionFiles();
    const now = Date.now();
    const maxAgeMs = Math.max(1000, Number(this.config.NEW_FILE_MAX_AGE_MS) || 120000);
    for (const [index, file] of files.entries()) {
      const seedTokenUsage = index === 0
        || now - file.mtimeMs <= maxAgeMs
        || localDayKey(file.mtimeMs) === localDayKey(now);
      this.trackFileBaseline(file.filePath, file.fileName, seedTokenUsage);
    }
    const snapshotEntry = Array.from(this.tracked.values())
      .find((entry) => entry.dailyTokenUsage);
    if (snapshotEntry) this.emitDailyTokenSnapshot(snapshotEntry);
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
      const initialTailBytes = Math.max(0, Number(this.config.INITIAL_TAIL_BYTES) || 0);
      const initialOffset = initialTailBytes > 0 && stat.size > initialTailBytes
        ? stat.size - initialTailBytes
        : 0;
      entry = this.createEntry(fileName, initialOffset, initialOffset > 0);
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

  pollSessions() {
    const files = this.listSessionFiles();
    const now = Date.now();
    const maxAgeMs = Math.max(1000, Number(this.config.NEW_FILE_MAX_AGE_MS) || 120000);

    for (const file of files) {
      if (!this.tracked.has(file.filePath) && now - file.mtimeMs > maxAgeMs) {
        this.trackFileBaseline(file.filePath, file.fileName);
        continue;
      }
      this.pollFile(file.filePath, file.fileName);
    }

    for (const filePath of this.tracked.keys()) {
      if (!fs.existsSync(filePath)) this.tracked.delete(filePath);
    }
  }
}

function execFileText(file, args, options) {
  return new Promise((resolve) => {
    execFile(file, args, {
      encoding: "utf8",
      maxBuffer: 4 * 1024 * 1024,
      ...options,
    }, (error, stdout) => {
      resolve(error ? "" : String(stdout || ""));
    });
  });
}

module.exports = ClaudeLogMonitor;
