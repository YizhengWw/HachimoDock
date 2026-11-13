'use strict'

// Read-only Hermes state poller (issue #28, M1).
//
// Polls ~/.hermes/state.db (SQLite/WAL, opened read-only) for new message
// rows past a persisted messages.id watermark, maps the newest signal of the
// followed session onto the deskpet state vocabulary via state-map.js, and
// POSTs it to the local HachimoDock bridge's /state endpoint. Hermes itself
// is never modified: no hook, no config write, no lock on its database.

const { DatabaseSync } = require('node:sqlite');
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const { messageEvent, reduce } = require('./state-map');
const { deviceIdentity } = require('./identity');

const DEFAULT_POLL_MS = 1000;
const MAX_ROWS_PER_POLL = 200;
const DEFAULT_PORTS = [23333, 23334, 23335, 23336, 23337];

const readJson = (file) => {
  try { return JSON.parse(fs.readFileSync(file, 'utf8')); } catch { return null; }
};
const writeJson = (file, value) => {
  try { fs.mkdirSync(path.dirname(file), { recursive: true }); fs.writeFileSync(file, JSON.stringify(value)); } catch { /* watermark loss only replays rows */ }
};

function defaultHttpRequest({ method, port, path: requestPath, body }) {
  return new Promise((resolve) => {
    const req = http.request({ host: '127.0.0.1', port, path: requestPath, method, timeout: 3000 }, (res) => {
      let text = '';
      res.on('data', (chunk) => { text += chunk; });
      res.on('end', () => resolve({ statusCode: res.statusCode, body: text }));
    });
    req.on('error', () => resolve({ statusCode: 0, body: '' }));
    req.on('timeout', () => { req.destroy(); resolve({ statusCode: 0, body: '' }); });
    if (body) req.write(body);
    req.end();
  });
}

class HermesMonitor {
  constructor({
    stateDbPath, runtimeJsonPath, lockPath, watermarkPath,
    port, pollIntervalMs = DEFAULT_POLL_MS, log = () => {}, httpRequest = defaultHttpRequest,
  } = {}) {
    this.stateDbPath = stateDbPath;
    this.runtimeJsonPath = runtimeJsonPath;
    this.lockPath = lockPath;
    this.watermarkPath = watermarkPath;
    this.pollIntervalMs = pollIntervalMs;
    this.log = log;
    this.httpRequest = httpRequest;
    this.db = null;
    const persisted = readJson(watermarkPath) || {};
    this.watermark = typeof persisted.watermark === 'number' ? persisted.watermark : null;
    this.lastReport = { sessionId: persisted.sessionId || '', state: persisted.state || '' };
    this.port = port || null;
    this.timer = null;
  }

  openDb() {
    if (this.db) return this.db;
    this.db = new DatabaseSync(this.stateDbPath, { readOnly: true });
    return this.db;
  }

  lockedSessionId() {
    return readJson(this.lockPath)?.sessionId || '';
  }

  contextFor(db, followed) {
    const session = db.prepare('SELECT source, ended_at, title, cwd FROM sessions WHERE id = ?').get(followed);
    const clarifyCalls = db.prepare(
      "SELECT tool_calls FROM messages WHERE session_id = ? AND role = 'assistant' AND tool_calls LIKE '%clarify%' ORDER BY id DESC LIMIT 100",
    ).all(followed).flatMap((row) => {
      try { return JSON.parse(row.tool_calls).map((call) => call?.id || call?.function?.id).filter(Boolean); } catch { return []; }
    });
    const answered = new Set(db.prepare(
      "SELECT tool_call_id FROM messages WHERE session_id = ? AND role = 'tool' AND tool_name = 'clarify'",
    ).all(followed).map((row) => row.tool_call_id));
    const now = Date.now() / 1000;
    return {
      session,
      pendingClarify: clarifyCalls.some((id) => !answered.has(id)),
      sessionEnded: Boolean(session?.ended_at),
      activeDelegation: db.prepare(
        "SELECT COUNT(*) AS c FROM async_delegations WHERE origin_session = ? AND completed_at IS NULL AND state NOT IN ('completed','error')",
      ).get(followed).c > 0,
      compressionActive: db.prepare(
        'SELECT COUNT(*) AS c FROM compression_locks WHERE session_id = ? AND (expires_at IS NULL OR expires_at > ?)',
      ).get(followed, now).c > 0,
      activeWorker: db.prepare(
        "SELECT COUNT(*) AS c FROM worker_executions WHERE session_id = ? AND status NOT IN ('completed','failed','cancelled','error')",
      ).get(followed).c > 0,
    };
  }

  async discoverPort() {
    if (this.port) return this.port;
    const runtime = readJson(this.runtimeJsonPath);
    const candidates = runtime
      ? [runtime.port, runtime.statePort, runtime.bridgePort, runtime.http?.port].filter(Number.isFinite)
      : [];
    for (const candidate of [...new Set(candidates), ...DEFAULT_PORTS]) {
      const probe = await this.httpRequest({ method: 'GET', port: candidate, path: '/state' });
      if (probe.statusCode === 200) { this.port = candidate; return candidate; }
    }
    this.port = DEFAULT_PORTS[0];
    return this.port;
  }

  async pollOnce() {
    let db;
    try { db = this.openDb(); } catch { this.db = null; return null; } // WAL checkpoint/rotation: retry next tick
    if (this.watermark === null) {
      this.watermark = db.prepare('SELECT COALESCE(MAX(id), 0) AS m FROM messages').get().m;
    }
    const rows = db.prepare(
      'SELECT id, session_id, role, content, tool_name, finish_reason, _compressed_summary FROM messages WHERE id > ? ORDER BY id ASC LIMIT ?',
    ).all(this.watermark, MAX_ROWS_PER_POLL);

    let followed = this.lockedSessionId();
    let event = null;
    let compactedThisPoll = false;
    if (rows.length) {
      followed = followed || rows[rows.length - 1].session_id;
      event = rows.map(messageEvent).reverse().find(Boolean) || null;
      compactedThisPoll = rows.some((row) => row._compressed_summary === 1);
    } else {
      // No new rows: sessions.ended_at can still flip without a message row,
      // so the followed session is re-evaluated for sleeping transitions.
      followed = followed || this.lastReport.sessionId;
      if (!followed) return null; // nothing followed yet; wait for the first row
    }

    const { session, ...ctx } = this.contextFor(db, followed);
    const mapped = reduce(event, { ...ctx, compactedThisPoll });
    if (mapped.state === this.lastReport.state && followed === this.lastReport.sessionId) return null; // transition-only

    const identity = deviceIdentity({ sessionId: followed, source: session?.source });
    const payload = {
      agent_id: 'hermes',
      state: mapped.state,
      event: mapped.event,
      session_id: followed,
      session_title: session?.title || '',
      cwd: session?.cwd || '',
      host: identity.host,
      hermes_source: identity.source,
      headless: true,
      source_pid: process.pid,
      ...(mapped.tool ? { tool_name: mapped.tool } : {}),
    };

    const port = await this.discoverPort();
    const res = await this.httpRequest({
      method: 'POST', port, path: '/state',
      body: JSON.stringify(payload),
    });
    if (res.statusCode !== 200) this.log('warn', 'POST /state failed', { port, statusCode: res.statusCode });

    if (rows.length) this.watermark = rows[rows.length - 1].id;
    this.lastReport = { sessionId: followed, state: mapped.state };
    writeJson(this.watermarkPath, { watermark: this.watermark, ...this.lastReport });
    return payload;
  }

  start() {
    this.stop();
    this.timer = setInterval(() => { this.pollOnce().catch((error) => this.log('warn', 'poll failed', { error: String(error?.message || error) })); }, this.pollIntervalMs);
    this.timer.unref?.();
    return this;
  }

  stop() {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }
}

module.exports = { HermesMonitor, DEFAULT_PORTS };
