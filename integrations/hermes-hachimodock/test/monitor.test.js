'use strict'

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { DatabaseSync } = require('node:sqlite');
const { HermesMonitor } = require('../hermes-monitor');

const SCHEMA = `
CREATE TABLE sessions (id TEXT PRIMARY KEY, source TEXT, ended_at REAL, title TEXT, cwd TEXT);
CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, content TEXT,
  tool_name TEXT, tool_calls TEXT, tool_call_id TEXT, finish_reason TEXT, _compressed_summary INTEGER);
CREATE TABLE async_delegations (origin_session TEXT, completed_at REAL, state TEXT);
CREATE TABLE compression_locks (session_id TEXT, expires_at REAL);
CREATE TABLE worker_executions (session_id TEXT, status TEXT);`;

function makeDb(dir, session = ['sess-1', 'cli', '语音草稿', 'D:/work']) {
  const dbPath = path.join(dir, 'state.db');
  const db = new DatabaseSync(dbPath);
  db.exec(SCHEMA);
  db.prepare('INSERT INTO sessions (id, source, title, cwd) VALUES (?, ?, ?, ?)').run(...session);
  return { db, dbPath };
}

test('monitor posts state transitions with identity, watermark and sleeping on end', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hermes-monitor-'));
  const { db, dbPath } = makeDb(dir);
  const posted = [];
  const httpRequest = async (req) => { posted.push(JSON.parse(req.body)); return { statusCode: 200, body: '{}' }; };
  const wm = path.join(dir, 'wm.json');
  const monitor = new HermesMonitor({ stateDbPath: dbPath, port: 23333, watermarkPath: wm, httpRequest });

  assert.equal(await monitor.pollOnce(), null); // seeds the watermark on empty history

  db.prepare('INSERT INTO messages (id, session_id, role, content, finish_reason) VALUES (?, ?, ?, ?, ?)')
    .run(1, 'sess-1', 'user', '跑一下', null);
  let payload = await monitor.pollOnce();
  assert.equal(payload.state, 'working');
  assert.equal(payload.agent_id, 'hermes');
  assert.equal(payload.session_id, 'sess-1');
  assert.equal(payload.hermes_source, 'cli');
  assert.match(payload.host, /^hermes-/);

  db.prepare('INSERT INTO messages (id, session_id, role, content, finish_reason, tool_calls) VALUES (?, ?, ?, ?, ?, ?)')
    .run(2, 'sess-1', 'assistant', '', 'tool_calls', '[{"id":"t1","type":"function","function":{"name":"terminal","arguments":"{}"}}]');
  payload = await monitor.pollOnce();
  assert.equal(payload.state, 'thinking');

  db.prepare('INSERT INTO messages (id, session_id, role, content, tool_name, tool_call_id, finish_reason) VALUES (?, ?, ?, ?, ?, ?, ?)')
    .run(3, 'sess-1', 'tool', '{"error":"ENOENT"}', 'terminal', 't1', null);
  payload = await monitor.pollOnce();
  assert.equal(payload.state, 'error');
  assert.equal(payload.tool_name, 'terminal');

  db.prepare('INSERT INTO messages (id, session_id, role, content, finish_reason) VALUES (?, ?, ?, ?, ?)')
    .run(4, 'sess-1', 'assistant', '好了', 'stop');
  db.prepare('UPDATE sessions SET ended_at = 1.0 WHERE id = ?').run('sess-1');
  payload = await monitor.pollOnce();
  assert.equal(payload.state, 'sleeping'); // ended_at beats the turn-end attention

  assert.equal(await monitor.pollOnce(), null); // unchanged: no repeat POST
  assert.equal(posted.length, 4);

  // A fresh monitor resumes from the persisted watermark instead of replaying history.
  const resumed = new HermesMonitor({ stateDbPath: dbPath, port: 23333, watermarkPath: wm, httpRequest });
  assert.equal(await resumed.pollOnce(), null);
  db.close();
});

test('lock file pins the followed session while another session is newer', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hermes-lock-'));
  const { db, dbPath } = makeDb(dir);
  db.prepare('INSERT INTO sessions (id, source, title, cwd) VALUES (?, ?, ?, ?)').run('sess-2', 'qqbot', '另一个', 'D:/other');
  db.prepare('INSERT INTO messages (id, session_id, role, content, finish_reason) VALUES (?, ?, ?, ?, ?)')
    .run(1, 'sess-1', 'user', '旧会话活动', null);
  const wm = path.join(dir, 'wm.json');
  const monitor = new HermesMonitor({ stateDbPath: dbPath, port: 23333, watermarkPath: wm, httpRequest: () => ({ statusCode: 200, body: '{}' }) });
  await monitor.pollOnce(); // seed watermark

  db.prepare('INSERT INTO messages (id, session_id, role, content, finish_reason) VALUES (?, ?, ?, ?, ?)')
    .run(2, 'sess-2', 'user', '新会话抢先', null);
  fs.writeFileSync(path.join(dir, 'lock.json'), JSON.stringify({ sessionId: 'sess-1' }));
  monitor.lockPath = path.join(dir, 'lock.json');
  const payload = await monitor.pollOnce();
  assert.equal(payload.session_id, 'sess-1'); // lock wins over newest activity
  db.close();
});

test('unanswered clarify tool call elevates the state to notification', async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hermes-clarify-'));
  const { db, dbPath } = makeDb(dir);
  const wm = path.join(dir, 'wm.json');
  const monitor = new HermesMonitor({ stateDbPath: dbPath, port: 23333, watermarkPath: wm, httpRequest: () => ({ statusCode: 200, body: '{}' }) });
  await monitor.pollOnce(); // seed

  db.prepare('INSERT INTO messages (id, session_id, role, content, finish_reason, tool_calls) VALUES (?, ?, ?, ?, ?, ?)')
    .run(1, 'sess-1', 'assistant', '', 'tool_calls', '[{"id":"c1","type":"function","function":{"name":"clarify","arguments":"{}"}}]');
  const payload = await monitor.pollOnce();
  assert.equal(payload.state, 'notification');

  // Answering the clarify clears the elevation on the next transition.
  db.prepare('INSERT INTO messages (id, session_id, role, content, tool_name, tool_call_id, finish_reason) VALUES (?, ?, ?, ?, ?, ?, ?)')
    .run(2, 'sess-1', 'tool', '{"output":"用户已回答"}', 'clarify', 'c1', null);
  const after = await monitor.pollOnce();
  assert.equal(after.state, 'working');
  db.close();
});
