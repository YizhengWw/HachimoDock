'use strict'
const test = require('node:test'), assert = require('node:assert/strict')
const { request } = require('node:http')
const { mkdtempSync, writeFileSync, chmodSync, symlinkSync, rmSync } = require('node:fs')
const { tmpdir } = require('node:os'), { join } = require('node:path')
const { BridgeServer } = require('../server')
const { readDescriptor, writeDescriptor, secret } = require('../capability')
const { startTestServer, startTestBridge, createFakeCtx, createFakeSession, createApprovalRequest, openSse, waitFor } = require('./helpers')
const ORIGIN = 'https://dsh.example.test:7443'
const bearer = (s, role) => ({ Authorization: `Bearer ${s.tokens[role]}` })
async function post(url, body, headers = {}) {
  const r = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json', ...headers }, body: JSON.stringify(body) })
  return { status: r.status, body: await r.json() }
}
async function raw(s, target, headers = {}, method = 'GET') {
  return new Promise((resolve, reject) => {
    const req = request({ hostname: '127.0.0.1', port: s.listeningPort, path: target, method, headers }, r => { r.resume(); r.on('end', () => resolve(r.statusCode)) }); req.on('error', reject); req.end()
  })
}

test('all sensitive routes reject unauthenticated and forged role/query credentials before dispatch', async t => {
  const { server: s, url } = await startTestServer(t)
  for (const p of ['/sessions', '/events?role=bridge', '/events?token=guess', '/inject', '/interrupt', '/voice/begin', '/voice/draft', '/voice/submit', '/approval/decision']) {
    const r = await fetch(url + p, p.startsWith('/events') || p === '/sessions' ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' })
    assert.ok([401, 403].includes(r.status), p); await r.text()
  }
  assert.equal(s.pending.size, 0); assert.equal(s.subscribers.size, 0); assert.equal(s.utterances.size, 0)
})
test('exact Origin and credential role both required; no wildcard and no origin reflection', async t => {
  const { server: s, url } = await startTestServer(t, { allowedOrigins: [ORIGIN] })
  for (const origin of ['https://evil.test', 'null', 'http://dsh.example.test:7443', 'https://dsh.example.test:7444', ORIGIN + '.evil']) {
    const r = await fetch(url + '/sessions', { headers: { ...bearer(s, 'bus'), Origin: origin } })
    assert.equal(r.status, 403); assert.equal(r.headers.get('access-control-allow-origin'), null); await r.text()
  }
  const nativeWithOrigin = await fetch(url + '/sessions', { headers: { ...bearer(s, 'bus'), Origin: ORIGIN } })
  assert.equal(nativeWithOrigin.status, 401); await nativeWithOrigin.text()
  const r = await fetch(url + '/health'); const p = await r.json()
  assert.deepEqual(p, { ok: true, version: 2 }); assert.equal(r.headers.get('access-control-allow-origin'), null)
})
test('Host, port, absolute-form and forwarded-host cannot bypass loopback policy', async t => {
  const { server: s } = await startTestServer(t)
  for (const host of ['evil.test', `localhost:${s.listeningPort}`, '127.0.0.1:1']) assert.equal(await raw(s, '/sessions', { ...bearer(s, 'bus'), Host: host }), 403)
  assert.equal(await raw(s, 'http://evil.test/sessions', bearer(s, 'bus')), 403)
  assert.equal(await raw(s, '//evil.test/sessions', bearer(s, 'bus')), 403)
  assert.equal(await raw(s, '/sessions', { ...bearer(s, 'bus'), Host: 'evil.test', 'X-Forwarded-Host': `127.0.0.1:${s.listeningPort}` }), 403)
})
test('preflight is exact and side effect free; actual request remains authenticated', async t => {
  const { server: s, url } = await startTestServer(t, { allowedOrigins: [ORIGIN] })
  const r = await fetch(url + '/approval/decision', { method: 'OPTIONS', headers: { Origin: ORIGIN, 'Access-Control-Request-Method': 'POST', 'Access-Control-Request-Headers': 'authorization, content-type' } })
  assert.equal(r.status, 204); assert.equal(r.headers.get('access-control-allow-origin'), ORIGIN); assert.equal(r.headers.get('vary'), 'Origin')
  assert.equal((await post(url + '/approval/decision', {}, { Origin: ORIGIN })).status, 401)
  assert.equal(s.pending.size, 0)
})
test('strict JSON type/schema/prototype and byte limit are enforced', async t => {
  const { server: s, url } = await startTestServer(t)
  for (const type of ['text/plain', 'application/x-www-form-urlencoded', 'multipart/form-data', 'application/problem+json']) {
    const r = await fetch(url + '/inject', { method: 'POST', headers: { ...bearer(s, 'bus'), 'Content-Type': type }, body: '{}' }); assert.equal(r.status, 415); await r.text()
  }
  for (const body of ['[]', 'null', '{"__proto__":{"allow":true}}', '{"constructor":{}}']) {
    const r = await fetch(url + '/inject', { method: 'POST', headers: { ...bearer(s, 'bus'), 'Content-Type': 'application/json' }, body }); assert.equal(r.status, 400); await r.text()
  }
  assert.equal(s.pending.size, 0)
})
test('browser role comes only from one-use exact-origin/session pairing and cannot answer approvals', async t => {
  const { server: s, url } = await startTestServer(t, { allowedOrigins: [ORIGIN], listSessions: () => [{ id: 'a' }, { id: 'b' }] })
  const ticket = await post(url + '/pair/ticket', { origin: ORIGIN, sessionId: 'a' }, bearer(s, 'voice'))
  assert.equal(ticket.status, 200)
  assert.equal((await post(url + '/pair', { code: ticket.body.code, sessionId: 'b' }, { Origin: ORIGIN })).status, 403)
  const p = await post(url + '/pair', { code: ticket.body.code, sessionId: 'a' }, { Origin: ORIGIN }); assert.equal(p.status, 200)
  assert.equal((await post(url + '/pair', { code: ticket.body.code, sessionId: 'a' }, { Origin: ORIGIN })).status, 403)
  const headers = { Origin: ORIGIN, Authorization: `Bearer ${p.body.token}` }
  assert.equal((await post(url + '/approval/decision', { requestId: 'x', decision: 'allow' }, headers)).status, 403)
  const missingOrigin = await fetch(url + '/events', { headers: { Authorization: headers.Authorization } }); assert.equal(missingOrigin.status, 401); await missingOrigin.text()
  const web = await openSse(url + '/events?role=relay', { headers }); t.after(() => web.close())
  assert.equal(s.hasEventSubscriber(), false)
})
test('exact closure/session/epoch/lease decisions; conflicts cannot grant a second time', async t => {
  const a = createFakeSession('a'), b = createFakeSession('b'), ctx = createFakeCtx({ sessions: [a, b] })
  const { bridge, url } = await startTestBridge(t, ctx); const s = bridge.server
  const stream = await openSse(url + '/events'); t.after(() => stream.close())
  const outA = bridge.handleApprovalRequest(createApprovalRequest(a)), outB = bridge.handleApprovalRequest(createApprovalRequest(b))
  const p = (await stream.next()).data, q = (await stream.next()).data
  const body = { requestId: p.requestId, decision: 'allow', epoch: p.epoch, sessionId: p.sessionId, lease: p.lease }
  for (const wrong of [{ sessionId: q.sessionId }, { epoch: secret() }, { lease: secret() }]) assert.equal((await post(url + '/approval/decision', { ...body, ...wrong }, bearer(s, 'relay'))).status, 409)
  assert.equal(s.pending.size, 2)
  assert.equal((await post(url + '/approval/decision', body, bearer(s, 'relay'))).status, 200)
  assert.equal((await post(url + '/approval/decision', { ...body, decision: 'deny' }, bearer(s, 'relay'))).status, 409)
  assert.equal(await outA, 'allowed-once'); assert.equal(s.pending.size, 1)
  bridge.server.cancelApproval(q.requestId); assert.equal(await outB, 'unavailable')
})
test('claimed approval never delegates/races native and already-aborted never publishes', async t => {
  const a = createFakeSession('a'), ctx = createFakeCtx({ sessions: [a] })
  const { bridge, url } = await startTestBridge(t, ctx); const stream = await openSse(url + '/events'); t.after(() => stream.close())
  let delegated = 0; const c = new AbortController(); c.abort()
  const seq = bridge.server.nextSeq
  assert.equal(await bridge.handleApprovalRequest(createApprovalRequest(a, { signal: c.signal }), () => { delegated++; return 'allowed-once' }), 'cancelled')
  assert.equal(bridge.server.nextSeq, seq)
  const out = bridge.handleApprovalRequest(createApprovalRequest(a), () => { delegated++; return 'allowed-once' })
  const p = (await stream.next()).data; bridge.server.cancelApproval(p.requestId)
  assert.equal(await out, 'unavailable'); assert.equal(delegated, 0)
})
test('relay detach wakes never-expiring approvals and old lease cannot decide new request', async t => {
  const { server: s, url } = await startTestServer(t)
  const first = await openSse(url + '/events')
  const old = s.requestApproval({ sessionId: 'a', timeoutMs: 99999 }); const p = (await first.next()).data
  first.close(); assert.equal(await old.outcome, 'unavailable')
  await waitFor(() => s.subscribers.size === 0)
  const second = await openSse(url + '/events'); t.after(() => second.close())
  const next = s.requestApproval({ sessionId: 'a', timeoutMs: 99999 }), q = (await second.next()).data
  assert.notEqual(q.lease, p.lease)
  assert.equal((await post(url + '/approval/decision', { requestId: next.requestId, sessionId: 'a', epoch: s.epoch, lease: p.lease, decision: 'allow' }, bearer(s, 'relay'))).status, 409)
  s.cancelApproval(next.requestId); await next.outcome
})
test('1000 cancellations retire timers/listeners and bound tombstones; no late pending publishes', async t => {
  const { server: s, url } = await startTestServer(t)
  const stream = await openSse(url + '/events'); t.after(() => stream.close())
  for (let i = 0; i < 1000; i++) {
    const c = new AbortController(), p = s.requestApproval({ sessionId: 'a', timeoutMs: 99999, signal: c.signal }); c.abort(); assert.equal(await p.outcome, 'cancelled'); if (i % 16 === 0) await new Promise(setImmediate)
  }
  assert.equal(s.pending.size, 0); assert.ok(s.resolved.size <= 256); assert.ok(s.events.length <= 512)
})
test('replay cursor cannot cross client/epoch; stale approval/voice commands are never replayed', async t => {
  const { server: s, url } = await startTestServer(t)
  for (const cursor of [`${secret()}:native-relay:0`, `${s.epoch}:other:0`]) {
    const r = await fetch(url + '/events?after=' + cursor, { headers: bearer(s, 'relay') }); assert.equal(r.status, 409); await r.text()
  }
  s.publish('approval', { requestId: 'old' }); s.publish('voice-submit', { utteranceId: 'old' }); s.publish('turn', { sessionId: 'a', phase: 'end' })
  const stream = await openSse(`${url}/events?after=${s.epoch}:native-relay:0`); t.after(() => stream.close())
  assert.equal((await stream.next()).event, 'turn')
})
test('slow subscriber queue is bounded and retired on overflow', async t => {
  const { server: s } = await startTestServer(t)
  let destroyed = false
  const sub = { role: 'relay', clientId: 'native-relay', lease: 'x', ended: false, res: { writableLength: 131072, write() { throw Error('must not queue') }, destroy() { destroyed = true }, end() {} } }
  s.subscribers.add(sub); s.writeToSubscriber(sub, { seq: 1, type: 'turn' })
  assert.equal(destroyed, true); assert.equal(s.subscribers.size, 0)
})
test('private bootstrap refuses symlinks/permissive files/directories; never reads real credentials', t => {
  const dir = mkdtempSync(join(tmpdir(), 'dsh-cap-')); t.after(() => rmSync(dir, { recursive: true, force: true })); chmodSync(dir, 0o700)
  const file = join(dir, 'capability.json'), d = { version: 2, baseUrl: 'http://127.0.0.1:17892', epoch: secret(), tokens: { relay: secret(), bus: secret(), voice: secret() } }
  writeDescriptor(d, file); assert.equal(readDescriptor(file).epoch, d.epoch)
  chmodSync(file, 0o644); assert.throws(() => readDescriptor(file)); chmodSync(file, 0o600)
  symlinkSync(file, join(dir, 'link.json')); assert.throws(() => readDescriptor(join(dir, 'link.json')))
  chmodSync(dir, 0o755); assert.throws(() => readDescriptor(file))
})
