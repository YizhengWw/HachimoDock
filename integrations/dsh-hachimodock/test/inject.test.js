'use strict'
const test = require('node:test')
const assert = require('node:assert/strict')
const { randomUUID } = require('node:crypto')
const { startTestServer, startTestBridge, openSse, postJson, requestJson, createFakeCtx, createFakeSession } = require('./helpers')
const body = (extra = {}) => ({ sessionId: 's1', requestId: randomUUID(), text: 'hello', ...extra })
const opts = b => ({ method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(b) })

test('inject transport preserves bounded token/tool and terminal adapter frames', async t => {
  const { url } = await startTestServer(t, { listSessions: () => [{ id: 's1' }], inject: async ({ emit, sessionId, requestId }) => {
    assert.equal(sessionId, 's1'); assert.ok(requestId)
    emit({ type: 'token', text: 'answer' }); emit({ type: 'tool', name: 'bash', phase: 'end', ok: false }); emit({ type: 'done', sessionId, tokens: 2, stopReason: 'admitted' })
  } })
  const sse = await openSse(url + '/inject', opts(body())); t.after(() => sse.close())
  assert.equal((await sse.next()).data.text, 'answer')
  assert.equal((await sse.next()).data.ok, false)
  assert.equal((await sse.next()).data.stopReason, 'admitted')
})
test('inject requires explicit existing session and requestId before adapter entry', async t => {
  let calls = 0
  const { url } = await startTestServer(t, { listSessions: () => [{ id: 's1' }], inject: async () => { calls++ } })
  for (const b of [body({ sessionId: 'auto' }), body({ sessionId: '' }), body({ sessionId: 'other' }), { text: 'hello' }, body({ requestId: '' })]) assert.equal((await postJson(url + '/inject', b)).status, 400)
  assert.equal(calls, 0)
})
test('inject rejects invalid text/shape/unknown fields before opening stream', async t => {
  const { url } = await startTestServer(t, { listSessions: () => [{ id: 's1' }] })
  for (const b of [body({ text: '' }), body({ text: ' '.repeat(4) }), body({ text: 'x'.repeat(32769) }), [], null, body({ extra: true })]) assert.equal((await postJson(url + '/inject', b)).status, 400)
})
test('adapter exception is an error frame, not a completion claim', async t => {
  const { url } = await startTestServer(t, { listSessions: () => [{ id: 's1' }], inject: async () => { throw new Error('failed') } })
  const sse = await openSse(url + '/inject', opts(body())); t.after(() => sse.close())
  assert.equal((await sse.next()).event, 'error')
})
test('inject deadline aborts a stalled adapter with an error', async t => {
  let signal
  const { url } = await startTestServer(t, { injectTimeoutMs: 30, listSessions: () => [{ id: 's1' }], inject: async a => { signal = a.signal; return new Promise(() => {}) } })
  const sse = await openSse(url + '/inject', opts(body())); t.after(() => sse.close())
  assert.equal((await sse.next()).event, 'error'); assert.equal(signal.aborted, true)
})
test('same requestId is never re-admitted by this bridge', async t => {
  let calls = 0
  const { url } = await startTestServer(t, { listSessions: () => [{ id: 's1' }], inject: async ({ emit }) => { calls++; emit({ type: 'done', sessionId: 's1' }) } })
  const b = body(), sse = await openSse(url + '/inject', opts(b)); await sse.next(); sse.close()
  assert.equal((await postJson(url + '/inject', b)).status, 409); assert.equal(calls, 1)
})
test('invalid inject text cannot consume or evict request receipts', async t => {
  let calls = 0
  const { server, url } = await startTestServer(t, { listSessions: () => [{ id: 's1' }], inject: async ({ emit }) => { calls++; emit({ type: 'done', sessionId: 's1' }) } })
  const req = body(), sse = await openSse(url + '/inject', opts(req)); await sse.next(); sse.close()
  for (let i = 0; i < 256; i++) assert.equal((await postJson(url + '/inject', body({ text: '' }))).status, 400)
  assert.equal(server.injecting.size, 1)
  assert.equal((await postJson(url + '/inject', req)).status, 409)
  assert.equal(calls, 1)
})
test('bounded receipt eviction preserves an in-flight inject request', async t => {
  const active = body(); let activeCalls = 0, release
  const pending = new Promise(resolve => { release = resolve })
  const { server, url } = await startTestServer(t, { listSessions: () => [{ id: 's1' }], inject: async ({ requestId, emit }) => {
    if (requestId === active.requestId) { activeCalls++; await pending }
    emit({ type: 'done', sessionId: 's1' })
  } })
  t.after(() => release())
  const first = await openSse(url + '/inject', opts(active)); t.after(() => first.close())
  try {
    for (let i = 0; i < 257; i++) {
      const sse = await openSse(url + '/inject', opts(body())); await sse.next(); sse.close()
    }
    assert.ok(server.injecting.size <= 256)
    assert.ok(server.injecting.has(active.requestId), 'in-flight receipt must survive completed-receipt churn')
    assert.equal((await postJson(url + '/inject', active)).status, 409)
    assert.equal(activeCalls, 1)
    release(); await first.next()
    assert.equal((await postJson(url + '/inject', active)).status, 409)
  } finally {
    release(); first.close()
  }
})
test('DSH prompt uses exact official SessionController queue request and reports only admission', async t => {
  const a = createFakeSession('s1'), b = createFakeSession('s2'); let received
  const ctx = createFakeCtx({ sessions: [a, b] })
  ctx.sessionController = { prompt: async (r, signal) => { received = r; assert.ok(signal); return { accepted: true } } }
  const { bridge, url } = await startTestBridge(t, ctx)
  assert.equal(bridge.resolveTargetSession('auto'), undefined)
  const req = body(), sse = await openSse(url + '/inject', opts(req)); t.after(() => sse.close())
  const frame = await sse.next()
  assert.deepEqual(received, { requestId: req.requestId, sessionId: 's1', mode: 'queue', content: [{ type: 'text', text: 'hello' }] })
  assert.equal(frame.data.stopReason, 'admitted'); assert.equal(frame.data.tokens, 0)
})
test('unsupported SessionController fails closed without guessed agent APIs', async t => {
  const session = createFakeSession('s1'), ctx = createFakeCtx({ sessions: [session] })
  const { url } = await startTestBridge(t, ctx)
  const sse = await openSse(url + '/inject', opts(body())); t.after(() => sse.close())
  assert.equal((await sse.next()).data.code, 'session-controller-unavailable')
})
test('lost prompt admission receipt is unknown and not blindly retried', async t => {
  const ctx = createFakeCtx({ sessions: [createFakeSession('s1')] }); let n = 0
  ctx.sessionController = { prompt: async () => { n++; throw new Error('lost response') } }
  const { url } = await startTestBridge(t, ctx)
  const req = body(), sse = await openSse(url + '/inject', opts(req)); t.after(() => sse.close())
  assert.equal((await sse.next()).data.code, 'delivery-unknown')
  assert.equal((await postJson(url + '/inject', req)).status, 409); assert.equal(n, 1)
})
