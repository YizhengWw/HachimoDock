"use strict";

const { execSync } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");
const defaults = require("./claude-code");

const MAX_MESSAGE_PREVIEW = 240;
const MAX_DISPLAY_TITLE = 22;

const TEXT_THINKING = "\u6b63\u5728\u601d\u8003";
const TEXT_REPLYING = "\u6b63\u5728\u56de\u590d";
const TEXT_DONE = "\u5df2\u5b8c\u6210";
const TEXT_SESSION_FALLBACK = "Claude \u4f1a\u8bdd";

function readNumber(value) {
  return typeof value === "number" && Number.isFinite(value) ? value : undefined;
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
  const totalTokens = readNumber(usage.total_tokens) ?? readNumber(usage.totalTokens);
  const parts = [inputTokens, outputTokens, cachedInputTokens, cacheCreationInputTokens].filter(Number.isFinite);
  const normalized = {
    inputTokens,
    outputTokens,
    cachedInputTokens,
    cacheCreationInputTokens,
    totalTokens: totalTokens ?? (parts.length ? parts.reduce((sum, value) => sum + value, 0) : undefined),
  };
  return Object.values(normalized).some(Number.isFinite) ? normalized : null;
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
    };
  }

  isRunningOnWindows() {
    const processNames = new Set(
      (Array.isArray(this.config.PROCESS_NAMES_WIN) ? this.config.PROCESS_NAMES_WIN : [])
        .map((name) => String(name).trim().toLowerCase())
        .filter(Boolean)
    );
    if (processNames.size === 0) return false;

    try {
      const output = execSync("tasklist /FO CSV /NH", {
        encoding: "utf8",
        timeout: 2000,
        windowsHide: true,
      });
      const lines = output.split(/\r?\n/);
      for (const line of lines) {
        const trimmed = line.trim();
        if (!trimmed) continue;
        const match = trimmed.match(/^"([^"]+)"/);
        if (!match || !match[1]) continue;
        if (processNames.has(match[1].toLowerCase())) return true;
      }
    } catch {}

    return false;
  }

  isRunningOnUnix() {
    const processNames = new Set(
      (Array.isArray(this.config.PROCESS_NAMES_UNIX) ? this.config.PROCESS_NAMES_UNIX : [])
        .map((name) => String(name).trim().toLowerCase())
        .filter(Boolean)
    );
    if (processNames.size === 0) return false;

    try {
      const output = execSync("ps -A -o comm=", {
        encoding: "utf8",
        timeout: 2000,
      });
      const lines = output.split(/\r?\n/);
      for (const line of lines) {
        const command = path.basename(line.trim()).toLowerCase();
        if (!command) continue;
        if (processNames.has(command)) return true;
      }
    } catch {}

    return false;
  }

  isRunning() {
    if (process.platform === "win32") return this.isRunningOnWindows();
    return this.isRunningOnUnix();
  }

  poll() {
    this.pollSessions();
    const running = this.isRunning();
    const now = Date.now();
    const heartbeatMs = Math.max(1000, Number(this.config.HEARTBEAT_MS) || 30000);
    const changed = this.lastRunning === null || this.lastRunning !== running;

    this.lastRunning = running;
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
    if (usage) entry.tokenUsage = usage;

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

    if (usage) {
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

  trackFileBaseline(filePath, fileName) {
    if (this.tracked.has(filePath)) return;
    try {
      const stat = fs.statSync(filePath);
      this.tracked.set(filePath, this.createEntry(fileName, stat.size, false));
    } catch {}
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

module.exports = ClaudeLogMonitor;
