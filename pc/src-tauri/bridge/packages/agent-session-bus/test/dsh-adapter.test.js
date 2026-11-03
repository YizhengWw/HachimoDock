'use strict'
const test = require('node:test'), assert = require('node:assert/strict'), http = require('node:http')
const fs = require('node:fs'), path = require('node:path'), os = require('node:os')
const { DshAdapter, _internal } = require('../src/adapters/dsh')
const { windowsAcl } = require('../src/adapters/windows-acl')
const token = 'a'.repeat(64), epoch = 'b'.repeat(64)
async function fixture(t, handler) {
  const server = http.createServer((req, res) => {
    assert.equal(req.headers.authorization, `Bearer ${token}`)
    Promise.resolve(handler(req, res)).catch(e => res.destroy(e))
  })
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dsh-bus-'))
  // Register cleanup before ACL setup: a failed secure setup must fail the
  // test without leaving a listening server behind.
  t.after(async () => { server.closeAllConnections(); await new Promise(r => server.close(r)); fs.rmSync(dir, { recursive: true, force: true }) })
  if (process.platform === 'win32') windowsAcl('prepare-directory', dir)
  else fs.chmodSync(dir, 0o700)
  const file = path.join(dir, 'capability.json'), baseUrl = `http://127.0.0.1:${server.address().port}`
  fs.writeFileSync(file, JSON.stringify({ version: 2, epoch, baseUrl, tokens: { bus: token } }), { mode: 0o600 })
  // Windows chmod does not establish a private DACL. Exercise the real fixed
  // PowerShell program and receipt verifier, just as the production writer does.
  if (process.platform === 'win32') windowsAcl('protect-file', file)
  return { file, dir, baseUrl, adapter: new DshAdapter({ env: { HACHIMODOCK_DSH_CAPABILITY_FILE: file } }) }
}
function sse(res, frames) { res.writeHead(200, { 'Content-Type': 'text/event-stream' }); for (const [e, d] of frames) res.write(`event: ${e}\ndata: ${JSON.stringify(d)}\n\n`); res.end() }
async function collect(a, request) { const events = []; for await (const e of a.inject(request)) events.push(e); return events }

test('endpoint rejects arbitrary hosts, userinfo, redirects targets, paths and invalid overrides', () => {
  for (const u of ['https://127.0.0.1:17892', 'http://evil.test:17892', 'http://127.0.0.1:17892/path', 'http://user@127.0.0.1:17892', 'not a url']) assert.throws(() => _internal.resolveBaseUrl(u))
})
test('availability authenticates actual descriptor endpoint via sessions, never public health', async t => {
  const { adapter } = await fixture(t, (req, res) => { assert.equal(req.url, '/sessions'); res.end(JSON.stringify({ version: 2, sessions: [] })) })
  assert.deepEqual(await adapter.isAvailable(), { ready: true, reason: null, version: 2 })
})
test('network budgets start after private descriptor verification, including synchronous Windows ACL checks', async t => {
  let requests = 0, verified = false, validations = 0
  const { adapter } = await fixture(t, (req, res) => {
    requests++
    if (req.url === '/inject') sse(res, [['done', { sessionId: 'a' }]])
    else res.end(JSON.stringify({ version: 2, sessions: [] }))
  })
  const auth = adapter._auth.bind(adapter), timeout = AbortSignal.timeout
  const budgets = []
  t.mock.method(adapter, '_auth', () => {
    assert.equal(verified, false)
    const headers = auth() // Real descriptor and, on Windows, real PowerShell ACL validation.
    verified = true; validations++
    return headers
  })
  t.mock.method(AbortSignal, 'timeout', milliseconds => {
    assert.equal(verified, true, 'no network deadline may start before trusted local validation')
    verified = false; budgets.push(milliseconds)
    return timeout.call(AbortSignal, milliseconds)
  })
  assert.equal((await adapter.isAvailable()).ready, true)
  assert.deepEqual(await adapter.listSessions(), [])
  assert.equal((await collect(adapter, { sessionId: 'a', text: 'synthetic' })).at(-1).kind, 'done')
  assert.deepEqual(budgets, [2000, 5000, 600000], 'existing network budgets stay unchanged')
  assert.equal(validations, 3)
  assert.equal(requests, 3)
})
test('unauthenticated health-style success is not readiness', async t => {
  let calls = 0
  const { adapter } = await fixture(t, (_, res) => { calls++; res.end(JSON.stringify({ ok: true, version: 2 })) })
  assert.equal((await adapter.isAvailable()).ready, false)
  assert.equal(calls, 1, 'the authorized endpoint must be reached before rejecting its readiness payload')
})
test('sessions keep compact display metadata and newest ordering', async t => {
  const { adapter } = await fixture(t, (_, res) => res.end(JSON.stringify({ version: 2, sessions: [
    { id: 'older', title: 'old', lastModified: 1 }, { id: 'newer', title: 'new', lastModified: 2, displayContent: 'display', state: 'stopped', transitionRevision: 3 }, { id: '' },
  ] })))
  const rows = await adapter.listSessions(); assert.deepEqual(rows.map(r => r.id), ['newer', 'older']); assert.equal(rows[0].displayContent, 'display'); assert.equal(rows[0].transitionRevision, 3)
})
test('inject sends exact session and random requestId once; admission remains admission', async t => {
  let calls = 0
  const { adapter } = await fixture(t, async (req, res) => {
    calls++; let body = ''; for await (const c of req) body += c
    const b = JSON.parse(body); assert.equal(b.sessionId, 'a'); assert.match(b.requestId, /^[a-f0-9-]{36}$/)
    sse(res, [['token', { text: 'answer' }], ['tool', { name: 'bash', phase: 'end', ok: false }], ['done', { sessionId: 'a', stopReason: 'admitted' }]])
  })
  const events = await collect(adapter, { sessionId: 'a', text: 'voice' }); assert.deepEqual(events.map(e => e.kind), ['token', 'tool', 'done']); assert.equal(events[2].stopReason, 'admitted'); assert.equal(calls, 1)
})
test('auto/new session cannot silently target the newest conversation', async t => {
  let calls = 0; const { adapter } = await fixture(t, (_, res) => { calls++; res.end() })
  assert.equal((await collect(adapter, { sessionId: 'auto', text: 'voice' }))[0].code, 'EXACT_SESSION_REQUIRED')
  await assert.rejects(adapter.openNew(), /explicit/); assert.equal(calls, 0)
})
test('stream EOF without receipt is delivery-unknown, never done', async t => {
  const { adapter } = await fixture(t, (_, res) => sse(res, [['token', { text: 'partial' }]]))
  assert.equal((await collect(adapter, { sessionId: 'a', text: 'voice' })).at(-1).code, 'DELIVERY_UNKNOWN')
})
test('mismatched target receipt and oversized unterminated frame are errors', async t => {
  let n = 0
  const { adapter } = await fixture(t, (_, res) => { if (++n === 1) sse(res, [['done', { sessionId: 'b' }]]); else { res.writeHead(200, { 'Content-Type': 'text/event-stream' }); res.end('x'.repeat(70000)) } })
  assert.equal((await collect(adapter, { sessionId: 'a', text: 'voice' }))[0].code, 'SESSION_MISMATCH')
  assert.equal((await collect(adapter, { sessionId: 'a', text: 'voice' }))[0].code, 'AGENT_STREAM_FAILED')
})
test('HTTP redirect is not followed and credentials do not leave endpoint', async t => {
  let calls = 0; const { adapter } = await fixture(t, (_, res) => { calls++; res.writeHead(302, { Location: 'http://evil.test/' }); res.end() })
  assert.equal((await collect(adapter, { sessionId: 'a', text: 'voice' }))[0].code, 'AGENT_UNAVAILABLE'); assert.equal(calls, 1)
})
test('unsafe descriptor and missing bootstrap fail closed without anonymous fallback', async t => {
  let calls = 0; const { adapter, file } = await fixture(t, (_, res) => { calls++; res.end() })
  if (process.platform === 'win32') {
    // A newly created child inherits the directory ACL but has no protected,
    // explicit file DACL. Keep valid synthetic descriptor bytes so rejection
    // proves the ACL gate, rather than malformed JSON or a missing endpoint.
    const descriptor = fs.readFileSync(file)
    fs.unlinkSync(file)
    fs.writeFileSync(file, descriptor, { mode: 0o600 })
    assert.throws(() => windowsAcl('verify-file', file))
  } else fs.chmodSync(file, 0o644)
  assert.equal((await adapter.isAvailable()).ready, false); assert.equal(calls, 0)
  fs.unlinkSync(file); assert.deepEqual(await adapter.listSessions(), []); assert.equal(calls, 0)
})
test('already-aborted request performs no network I/O', async t => {
  let calls = 0; const { adapter } = await fixture(t, (_, res) => { calls++; res.end() })
  const c = new AbortController(); c.abort(); const events = await collect(adapter, { sessionId: 'a', text: 'voice', signal: c.signal }); assert.equal(events[0].code, 'CANCELLED'); assert.equal(calls, 0)
})
