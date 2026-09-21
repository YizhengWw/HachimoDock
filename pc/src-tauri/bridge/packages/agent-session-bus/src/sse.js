"use strict";

// Caller passes `extraHeaders` to layer CORS / cache-control on top of the
// SSE-mandatory headers. Server.js threads its corsHeaders(req) through here
// because the Tauri webview blocks any 127.0.0.1 stream that lacks an
// Access-Control-Allow-Origin even when the body is plain SSE.
function openSseStream(res, extraHeaders = {}) {
  if (res.headersSent) {
    throw new Error("openSseStream called after headers were already sent");
  }
  res.writeHead(200, {
    "Content-Type": "text/event-stream; charset=utf-8",
    "Cache-Control": "no-cache, no-transform",
    Connection: "keep-alive",
    "X-Accel-Buffering": "no",
    ...extraHeaders,
  });
  if (typeof res.flushHeaders === "function") {
    try { res.flushHeaders(); } catch { /* ignore */ }
  }
  res.write(": agent-session-bus stream\n\n");
}

function writeSseEvent(res, event, data) {
  if (res.writableEnded || res.destroyed) return false;
  const payload = typeof data === "string" ? data : JSON.stringify(data);
  res.write(`event: ${event}\n`);
  for (const line of payload.split(/\r?\n/)) {
    res.write(`data: ${line}\n`);
  }
  res.write("\n");
  return true;
}

function writeSseComment(res, text) {
  if (res.writableEnded || res.destroyed) return false;
  for (const line of String(text).split(/\r?\n/)) {
    res.write(`: ${line}\n`);
  }
  res.write("\n");
  return true;
}

function endSseStream(res) {
  if (!res.writableEnded) {
    try { res.end(); } catch { /* ignore */ }
  }
}

function startSseHeartbeat(res, intervalMs = 15000) {
  const timer = setInterval(() => {
    writeSseComment(res, "keep-alive");
  }, intervalMs);
  if (typeof timer.unref === "function") timer.unref();
  return () => clearInterval(timer);
}

module.exports = {
  openSseStream,
  writeSseEvent,
  writeSseComment,
  endSseStream,
  startSseHeartbeat,
};
