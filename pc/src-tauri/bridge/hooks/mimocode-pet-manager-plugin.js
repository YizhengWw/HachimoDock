import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const AGENT_ID = "mimocode";
const DEFAULT_PORT = 23333;
const MIN_PORT = 23333;
const MAX_PORT = 23337;
const POST_TIMEOUT_MS = 700;
const SPEAKING_THROTTLE_MS = 140;
const FAILED_POST_BACKOFF_MS = 2000;
const DEBUG = process.env.PET_MANAGER_MIMOCODE_DEBUG === "1";
const RUNTIME_PATH = process.env.PET_MANAGER_RUNTIME_PATH
  || path.join(os.homedir(), ".clawd", "runtime.json");

const sessionInfo = new Map();
const activeSessions = new Set();
const terminalSessions = new Set();
const lastTextBySession = new Map();
const speakingTimers = new Map();
let latestActiveSessionID = "";
let cachedPort = null;
let cachedPortAt = 0;
let postBackoffUntil = 0;
let discoveryPromise = null;
let postChain = Promise.resolve();

function debugEvent(kind, value = {}) {
  if (!DEBUG) return;
  console.error(`[pet-manager-mimocode] ${JSON.stringify({ kind, ...value })}`);
}

function compactText(value, maxLength = 240) {
  const text = typeof value === "string"
    ? value.replace(/\s+/g, " ").trim()
    : "";
  if (!text) return "";
  return text.length > maxLength
    ? `${text.slice(0, Math.max(0, maxLength - 3))}...`
    : text;
}

function firstLine(value, maxLength = 120) {
  const text = compactText(value, maxLength * 3);
  if (!text) return "";
  return compactText(text.split(/[。！？!?\n]/)[0] || text, maxLength);
}

function rememberSession(info, fallbackDirectory = "") {
  if (!info || typeof info !== "object" || typeof info.id !== "string" || !info.id) return;
  const previous = sessionInfo.get(info.id) || {};
  sessionInfo.set(info.id, {
    id: info.id,
    title: compactText(info.title, 160) || previous.title || "",
    directory: compactText(info.directory, 500) || previous.directory || fallbackDirectory,
  });
}

function sessionMetadata(sessionID, fallbackDirectory = "") {
  const info = sessionInfo.get(sessionID) || {};
  return {
    sessionTitle: compactText(info.title, 160),
    cwd: compactText(info.directory, 500) || fallbackDirectory,
  };
}

function updateSessionFallbackTitle(sessionID, query, fallbackDirectory) {
  const title = firstLine(query, 120);
  const previous = sessionInfo.get(sessionID) || {};
  sessionInfo.set(sessionID, {
    id: sessionID,
    title: previous.title || title,
    directory: previous.directory || fallbackDirectory,
  });
}

function readRuntimePort() {
  const now = Date.now();
  if (cachedPort && now - cachedPortAt < 1000) return cachedPort;
  cachedPortAt = now;
  const explicitPort = Number(process.env.PET_MANAGER_BRIDGE_PORT);
  if (Number.isInteger(explicitPort) && explicitPort > 0 && explicitPort <= 65535) {
    cachedPort = explicitPort;
    return cachedPort;
  }
  try {
    const config = JSON.parse(fs.readFileSync(RUNTIME_PATH, "utf8"));
    const port = Number(config?.port);
    cachedPort = Number.isInteger(port) && port >= MIN_PORT && port <= MAX_PORT
      ? port
      : DEFAULT_PORT;
  } catch {
    cachedPort = null;
  }
  return cachedPort;
}

async function probePort(port) {
  try {
    const response = await fetch(`http://127.0.0.1:${port}/state`, {
      method: "GET",
      signal: AbortSignal.timeout(300),
    });
    if (!response.ok) return false;
    if (response.headers.get("x-clawd-server") === "clawd-on-desk") return true;
    const body = await response.json().catch(() => null);
    return body?.app === "clawd-on-desk";
  } catch {
    return false;
  }
}

async function discoverBridgePort() {
  const configured = readRuntimePort();
  if (process.env.PET_MANAGER_BRIDGE_PORT && configured) return configured;
  if (configured && await probePort(configured)) return configured;
  for (let port = MIN_PORT; port <= MAX_PORT; port += 1) {
    if (port === configured) continue;
    if (await probePort(port)) {
      cachedPort = port;
      cachedPortAt = Date.now();
      return port;
    }
  }
  return null;
}

async function resolveBridgePort() {
  if (discoveryPromise) return discoveryPromise;
  discoveryPromise = discoverBridgePort().finally(() => {
    discoveryPromise = null;
  });
  return discoveryPromise;
}

async function postState(payload) {
  if (Date.now() < postBackoffUntil) return;
  const port = await resolveBridgePort();
  if (!port) {
    postBackoffUntil = Date.now() + FAILED_POST_BACKOFF_MS;
    return;
  }

  try {
    const response = await fetch(`http://127.0.0.1:${port}/state`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
      signal: AbortSignal.timeout(POST_TIMEOUT_MS),
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
  } catch {
    cachedPort = null;
    postBackoffUntil = Date.now() + FAILED_POST_BACKOFF_MS;
  }
}

function buildStatePayload({
  state,
  event,
  sessionID,
  fallbackDirectory,
  content,
  toolName,
  sessionTitleExplicit = true,
}) {
  const metadata = sessionMetadata(sessionID, fallbackDirectory);
  return {
    agent_id: AGENT_ID,
    state,
    event,
    session_id: sessionID || undefined,
    session_title: metadata.sessionTitle || undefined,
    session_title_explicit: Boolean(metadata.sessionTitle) && sessionTitleExplicit,
    cwd: metadata.cwd || undefined,
    display_title: metadata.sessionTitle || "MiMoCode",
    display_content: compactText(content, 320) || undefined,
    last_assistant_message: state === "done" ? compactText(content, 2400) || undefined : undefined,
    tool_name: compactText(toolName, 100) || undefined,
    source_pid: process.pid,
  };
}

function publishState(input) {
  const payload = buildStatePayload(input);
  const sessionID = payload.session_id || "";
  if (payload.state === "done" || payload.state === "error") {
    if (sessionID && terminalSessions.has(sessionID)) return Promise.resolve();
    if (sessionID) terminalSessions.add(sessionID);
  } else if (sessionID && terminalSessions.has(sessionID)) {
    return Promise.resolve();
  }
  postChain = postChain
    .then(() => postState(payload))
    .catch(() => {});
  return postChain;
}

function markSessionActive(sessionID) {
  if (!sessionID) return;
  terminalSessions.delete(sessionID);
  activeSessions.add(sessionID);
  latestActiveSessionID = sessionID;
}

function publishSpeaking(sessionID, fallbackDirectory, text) {
  const content = compactText(text, 320);
  if (!sessionID || !content || !activeSessions.has(sessionID)) return;
  lastTextBySession.set(sessionID, compactText(text, 2400));
  if (speakingTimers.has(sessionID)) return;

  const timer = setTimeout(() => {
    speakingTimers.delete(sessionID);
    publishState({
      state: "speaking",
      event: "message.part.updated",
      sessionID,
      fallbackDirectory,
      content: lastTextBySession.get(sessionID) || content,
    });
  }, SPEAKING_THROTTLE_MS);
  timer.unref?.();
  speakingTimers.set(sessionID, timer);
}

function publishIdleCompletion(sessionID, fallbackDirectory, event) {
  if (!sessionID) return;
  activeSessions.delete(sessionID);
  const speakingTimer = speakingTimers.get(sessionID);
  if (speakingTimer) clearTimeout(speakingTimer);
  speakingTimers.delete(sessionID);
  const finalText = lastTextBySession.get(sessionID) || "";
  lastTextBySession.delete(sessionID);
  return publishState({
    state: "done",
    event,
    sessionID,
    fallbackDirectory,
    content: finalText || "已完成",
  });
}

function errorText(error) {
  if (!error) return "执行失败";
  if (typeof error === "string") return compactText(error, 320);
  if (typeof error.message === "string") return compactText(error.message, 320);
  if (typeof error.data?.message === "string") return compactText(error.data.message, 320);
  return compactText(JSON.stringify(error), 320) || "执行失败";
}

function questionText(properties) {
  const first = Array.isArray(properties?.questions) ? properties.questions[0] : null;
  return compactText(first?.question || first?.header || "等待回答", 240);
}

function handleEvent(event, fallbackDirectory) {
  if (!event || typeof event !== "object") return;
  const properties = event.properties && typeof event.properties === "object"
    ? event.properties
    : {};
  const sessionID = typeof properties.sessionID === "string" ? properties.sessionID : "";

  if (event.type === "session.created" || event.type === "session.updated") {
    rememberSession(properties.info, fallbackDirectory);
    return;
  }
  if (event.type === "session.deleted") {
    sessionInfo.delete(sessionID);
    activeSessions.delete(sessionID);
    lastTextBySession.delete(sessionID);
    return;
  }
  if (event.type === "session.cwd" && sessionID) {
    const previous = sessionInfo.get(sessionID) || {};
    sessionInfo.set(sessionID, {
      id: sessionID,
      title: previous.title || "",
      directory: compactText(properties.cwd, 500) || previous.directory || fallbackDirectory,
    });
    return;
  }
  if (event.type === "session.status" && sessionID) {
    const statusType = properties.status?.type;
    if (statusType === "busy") {
      markSessionActive(sessionID);
      publishState({
        state: "working",
        event: event.type,
        sessionID,
        fallbackDirectory,
        content: compactText(properties.status?.message, 240) || "正在处理",
      });
    } else if (statusType === "retry") {
      markSessionActive(sessionID);
      publishState({
        state: "working",
        event: "session.retry",
        sessionID,
        fallbackDirectory,
        content: `正在重试（第 ${Number(properties.status?.attempt) || 1} 次）`,
      });
    } else if (statusType === "idle") {
      return publishIdleCompletion(sessionID, fallbackDirectory, event.type);
    }
    return;
  }
  if (event.type === "message.updated" && sessionID) {
    const info = properties.info;
    if (info?.role !== "assistant" || !info.time?.completed) return;
    if (info.error) {
      activeSessions.delete(sessionID);
      return publishState({
        state: "error",
        event: event.type,
        sessionID,
        fallbackDirectory,
        content: errorText(info.error),
      });
    }
    if (info.finish === "tool-calls") return;
    return publishIdleCompletion(sessionID, fallbackDirectory, event.type);
  }
  if (event.type === "session.idle" && sessionID) {
    return publishIdleCompletion(sessionID, fallbackDirectory, event.type);
  }
  if (event.type === "message.part.updated" && sessionID) {
    const part = properties.part;
    if (part?.type === "text") {
      publishSpeaking(sessionID, fallbackDirectory, part.text);
      return;
    }
    if (part?.type === "tool") {
      const toolStatus = part.state?.status;
      const toolName = compactText(part.tool, 100);
      const toolTitle = compactText(part.state?.title, 180);
      if (toolStatus === "pending" || toolStatus === "running") {
        publishState({
          state: "working",
          event: `tool.${toolStatus}`,
          sessionID,
          fallbackDirectory,
          content: toolTitle || (toolName ? `正在执行 ${toolName}` : "正在执行工具"),
          toolName,
        });
      } else if (toolStatus === "error") {
        publishState({
          state: "error",
          event: "tool.error",
          sessionID,
          fallbackDirectory,
          content: compactText(part.state?.error, 320) || "工具执行失败",
          toolName,
        });
      }
    }
    return;
  }
  if (event.type === "permission.asked" && sessionID) {
    publishState({
      state: "waiting_user",
      event: event.type,
      sessionID,
      fallbackDirectory,
      content: properties.permission
        ? `等待授权：${compactText(properties.permission, 160)}`
        : "等待授权",
    });
    return;
  }
  if (event.type === "question.asked" && sessionID) {
    publishState({
      state: "waiting_user",
      event: event.type,
      sessionID,
      fallbackDirectory,
      content: questionText(properties),
    });
    return;
  }
  if (event.type === "bash.interactive.asked") {
    const targetSessionID = sessionID || latestActiveSessionID;
    if (!targetSessionID) return;
    publishState({
      state: "waiting_user",
      event: event.type,
      sessionID: targetSessionID,
      fallbackDirectory,
      content: compactText(properties.description, 240) || "等待终端输入",
      toolName: "bash",
    });
    return;
  }
  if (event.type === "session.retry.attempt" && sessionID) {
    publishState({
      state: "working",
      event: event.type,
      sessionID,
      fallbackDirectory,
      content: `正在重试（${Number(properties.attempt) || 1}/${Number(properties.maxAttempts) || "?"}）`,
    });
    return;
  }
  if (event.type === "session.error") {
    const targetSessionID = sessionID || latestActiveSessionID;
    if (!targetSessionID) return;
    activeSessions.delete(targetSessionID);
    return publishState({
      state: "error",
      event: event.type,
      sessionID: targetSessionID,
      fallbackDirectory,
      content: errorText(properties.error),
    });
  }
}

export async function PetManagerMiMoCodePlugin({ directory }) {
  const fallbackDirectory = typeof directory === "string" ? directory : "";

  return {
    event: async ({ event }) => {
      debugEvent("event", {
        type: event?.type,
        sessionID: event?.properties?.sessionID || "",
        status: event?.properties?.status?.type || "",
      });
      await handleEvent(event, fallbackDirectory);
    },
    "session.pre": async (input) => {
      debugEvent("session.pre", { sessionID: input?.sessionID || "" });
      if (!input?.sessionID) return;
      markSessionActive(input.sessionID);
      publishState({
        state: "working",
        event: "session.pre",
        sessionID: input.sessionID,
        fallbackDirectory,
        content: "正在处理",
      });
    },
    "session.userQuery.pre": async (input) => {
      debugEvent("session.userQuery.pre", { sessionID: input?.sessionID || "" });
      if (!input?.sessionID) return;
      updateSessionFallbackTitle(input.sessionID, input.query, fallbackDirectory);
      markSessionActive(input.sessionID);
      publishState({
        state: "working",
        event: "session.userQuery.pre",
        sessionID: input.sessionID,
        fallbackDirectory,
        content: "正在思考",
        sessionTitleExplicit: false,
      });
    },
    "session.post": async (input) => {
      debugEvent("session.post", {
        sessionID: input?.sessionID || "",
        outcome: input?.outcome || "",
      });
      if (!input?.sessionID) return;
      activeSessions.delete(input.sessionID);
      const speakingTimer = speakingTimers.get(input.sessionID);
      if (speakingTimer) clearTimeout(speakingTimer);
      speakingTimers.delete(input.sessionID);
      const finalText = compactText(input.finalText, 2400)
        || lastTextBySession.get(input.sessionID)
        || "";
      lastTextBySession.delete(input.sessionID);
      if (input.outcome === "completed") {
        await publishState({
          state: "done",
          event: "session.post",
          sessionID: input.sessionID,
          fallbackDirectory,
          content: finalText || "已完成",
        });
        return;
      }
      if (input.outcome === "cancelled") {
        await publishState({
          state: "waiting_user",
          event: "session.cancelled",
          sessionID: input.sessionID,
          fallbackDirectory,
          content: compactText(input.error, 320) || "任务已取消",
        });
        return;
      }
      await publishState({
        state: "error",
        event: "session.post",
        sessionID: input.sessionID,
        fallbackDirectory,
        content: compactText(input.error, 320) || "执行失败",
      });
    },
  };
}
