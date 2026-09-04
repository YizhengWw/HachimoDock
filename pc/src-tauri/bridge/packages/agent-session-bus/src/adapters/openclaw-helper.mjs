#!/usr/bin/env node
// OpenClaw bridge helper. Spawned by OpenClawAdapter to invoke
// `openclaw/dist/plugin-sdk/agent-runtime.js#agentCommand` in-process so we
// can stream events back over stdout. This is a slimmed-down twin of
// pet-claw's helper (`pet-claw-agent-helper.mjs`); we ship our own copy
// because the bus owns the contract and can't depend on pet-claw being
// installed alongside.
//
// Wire shape (one event per stdout line, prefixed with `@@AGENT_BUS@@`):
//   { type: "session", id: "<sid>" }                         once we know it
//   { type: "event",   event: { kind: "token", text: "..." } }
//   { type: "event",   event: { kind: "tool", name, phase, ok? } }
//   { type: "result",  text: "...full reply...", meta }
//   { type: "error",   error: { name, message, stack? } }
//
// Stdin: a single JSON document
//   { message, agentId, sessionId, model, provider, runId? }

import fs from "node:fs/promises";
import path from "node:path";
import { pathToFileURL } from "node:url";

const EVENT_PREFIX = "@@AGENT_BUS@@";

function emit(record) {
  process.stdout.write(`${EVENT_PREFIX}${JSON.stringify(record)}\n`);
}

function readStdin() {
  return new Promise((resolve, reject) => {
    let buffer = "";
    process.stdin.setEncoding("utf8");
    process.stdin.on("data", (chunk) => { buffer += chunk; });
    process.stdin.on("end", () => resolve(buffer));
    process.stdin.on("error", reject);
  });
}

function findExportedFunction(moduleNamespace, functionName) {
  if (!moduleNamespace || typeof moduleNamespace !== "object") return null;
  for (const value of Object.values(moduleNamespace)) {
    if (typeof value === "function" && value.name === functionName) {
      return value;
    }
  }
  return null;
}

async function resolveOnAgentEvent(agentRuntimeModule) {
  const distDir = path.resolve(path.dirname(agentRuntimeModule), "..");
  let entries;
  try {
    entries = await fs.readdir(distDir);
  } catch {
    return null;
  }
  for (const name of entries) {
    if (!/^(agent-events-|pi-embedded-).*\.js$/.test(name)) continue;
    try {
      const ns = await import(pathToFileURL(path.join(distDir, name)).href);
      const fn = findExportedFunction(ns, "onAgentEvent");
      if (fn) return fn;
    } catch {
      // OpenClaw can run without the embedded event stream adapter.
    }
  }
  return null;
}

function normaliseEvent(event) {
  // OpenClaw's agent-event stream emits objects roughly like:
  //   { runId, type: "agent.message.delta" | "agent.message.full" |
  //                  "tool.call.start" | "tool.call.end" | ..., payload: {...} }
  // We translate the relevant ones to AgentEvent shape. Anything we don't
  // recognise we forward verbatim wrapped in `{ kind: "raw", event }` so
  // the parent can log/debug without our schema getting in the way.
  if (!event || typeof event !== "object") return null;
  const t = typeof event.type === "string" ? event.type : "";
  const payload = event.payload && typeof event.payload === "object" ? event.payload : {};

  if (t.endsWith(".delta") && typeof payload.text === "string" && payload.text) {
    return { kind: "token", text: payload.text };
  }
  if (t.endsWith(".full") && typeof payload.text === "string" && payload.text) {
    // Prefer .delta when both arrive; .full is a complete-message snapshot.
    return null;
  }
  if (t === "tool.call.start") {
    return { kind: "tool", name: payload.name || payload.tool || "unknown", phase: "start", input: payload.input };
  }
  if (t === "tool.call.end") {
    return { kind: "tool", name: payload.name || payload.tool || "unknown", phase: "end", ok: !payload.error };
  }
  if (t === "agent.error" || t === "error") {
    return { kind: "error", code: payload.code || "AGENT_ERROR", message: payload.message || "openclaw error" };
  }
  return null;
}

async function main() {
  const agentRuntimeModule = process.env.OPENCLAW_RUNTIME_MODULE_RESOLVED;
  if (!agentRuntimeModule) {
    throw new Error("OPENCLAW_RUNTIME_MODULE_RESOLVED 未设置");
  }

  const rawInput = await readStdin();
  const input = rawInput.trim() ? JSON.parse(rawInput) : {};
  if (typeof input.message !== "string" || !input.message.trim()) {
    throw new Error("message 不能为空");
  }

  const runId = input.runId || `bus-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;

  const { agentCommand } = await import(pathToFileURL(agentRuntimeModule).href);
  const onAgentEvent = await resolveOnAgentEvent(agentRuntimeModule);

  const runtime = {
    log: () => {},
    error: (...args) => { console.error("[openclaw-helper]", ...args); },
    exit: (code) => { if (typeof code === "number") process.exitCode = code; },
  };

  let sessionEmitted = Boolean(input.sessionId);
  if (sessionEmitted) emit({ type: "session", id: input.sessionId });

  // Track whether the upstream live event stream produced any token
  // events at all. When openclaw is built without `pi-embedded-*` the
  // event subscription is a no-op (`onAgentEvent` resolves to null) and
  // the only signal we get back is the final `result.payloads` blob.
  // In that fallback path we want to *synthesise* a streaming token
  // experience by chunking the final text into clauses (so TTS can
  // start speaking before the full reply is buffered downstream). When
  // a real stream IS available we leave it alone.
  let tokenEmittedFromStream = false;

  const unsubscribe = typeof onAgentEvent === "function"
    ? onAgentEvent((event) => {
        if (event?.runId !== runId) return;
        if (!sessionEmitted && typeof event?.sessionId === "string" && event.sessionId) {
          sessionEmitted = true;
          emit({ type: "session", id: event.sessionId });
        }
        const normalised = normaliseEvent(event);
        if (normalised) {
          if (normalised.kind === "token") tokenEmittedFromStream = true;
          emit({ type: "event", event: normalised });
        }
      })
    : () => {};

  let result;
  try {
    result = await agentCommand({
      message: input.message,
      agentId: input.agentId || "main",
      sessionId: input.sessionId,
      sessionKey: input.sessionKey,
      provider: input.provider,
      model: input.model,
      extraSystemPrompt: input.extraSystemPrompt,
      timeout: input.timeout,
      thinking: input.thinking,
      verbose: input.verbose,
      deliver: false,
      json: false,
      runId,
      streamParams: input.streamParams,
    }, runtime);
  } finally {
    try { unsubscribe(); } catch { /* ignore */ }
  }

  const fullText = Array.isArray(result?.payloads)
    ? result.payloads
        .map((p) => typeof p?.text === "string" ? p.text : "")
        .filter(Boolean)
        .join("\n")
        .trim()
    : "";
  const finalSid =
    (result?.meta && typeof result.meta.sessionId === "string" && result.meta.sessionId) ||
    (typeof result?.sessionId === "string" && result.sessionId) ||
    "";

  if (finalSid && !sessionEmitted) {
    sessionEmitted = true;
    emit({ type: "session", id: finalSid });
  }

  // Streaming-fallback: when no `pi-embedded` adapter wired tokens to
  // us during the run, fan the final text out as clause-sized token
  // events so TTS starts speaking on the first sentence rather than
  // waiting for the whole turn. We pass an empty `text` on the result
  // to suppress OpenClawAdapter's "no tokens? echo result.text once"
  // fallback, which would otherwise duplicate the full reply.
  if (!tokenEmittedFromStream && fullText) {
    for (const chunk of splitForTTS(fullText)) {
      emit({ type: "event", event: { kind: "token", text: chunk } });
    }
    emit({
      type: "result",
      text: "",
      meta: result?.meta || null,
      sessionId: finalSid || null,
    });
    return;
  }

  emit({
    type: "result",
    text: fullText,
    meta: result?.meta || null,
    sessionId: finalSid || null,
  });
}

/**
 * Break a complete reply into TTS-friendly chunks. We split on sentence-
 * terminating punctuation (Chinese 。！？ + Western .!? + line breaks)
 * but preserve the punctuation so each chunk reads naturally on its own.
 *
 * Chunks are merged when they're tiny (< 4 chars) to avoid spitting out
 * one-character utterances like "。" — the TTS engine handles longer
 * fragments far more cleanly.
 *
 * @param {string} text
 * @returns {string[]}
 */
function splitForTTS(text) {
  if (!text || typeof text !== "string") return [];
  const raw = [];
  let buf = "";
  for (const ch of text) {
    buf += ch;
    if (/[。！？.!?\n]/.test(ch)) {
      if (buf.trim()) raw.push(buf);
      buf = "";
    }
  }
  if (buf.trim()) raw.push(buf);
  // Coalesce tiny fragments forward so we don't emit "。" alone.
  const out = [];
  for (const piece of raw) {
    if (out.length && piece.trim().length < 4) {
      out[out.length - 1] += piece;
    } else {
      out.push(piece);
    }
  }
  return out.map((s) => s.replace(/\n+/g, "\n").trim()).filter(Boolean);
}

main().catch((error) => {
  emit({
    type: "error",
    error: {
      name: error?.name || "Error",
      message: error?.message || String(error),
      stack: error?.stack,
    },
  });
  process.exitCode = 1;
});
