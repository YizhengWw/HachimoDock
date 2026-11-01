'use strict'

/**
 * POST /inject SSE contract: token/tool/done/error events.
 *
 * Two layers:
 *   1. transport with a fake `inject` dep (server.js alone),
 *   2. end-to-end with the real DSH adapter over a fake Cordis context.
 */

const test = require('node:test')
const assert = require('node:assert/strict')

const {
  createFakeCtx,
  createFakeSession,
  createFakeAgent,
  startTestServer,
  startTestBridge,
  openSse,
  postJson,
  requestJson,
  waitFor,
} = require('./helpers')

test('POST /inject streams token, tool, and done frames from the adapter', async (t) => {
  let received
  const { url } = await startTestServer(t, {
    inject: async ({ sessionId, text, emit, signal }) => {
      received = { sessionId, text, aborted: signal.aborted }
      emit({ type: 'token', text: 'Hello ' })
      emit({ type: 'token', text: 'world' })
      emit({ type: 'tool', name: 'bash', phase: 'start', ok: true })
      emit({ type: 'tool', name: 'bash', phase: 'end', ok: true })
      emit({ type: 'done', sessionId: 'session-x', tokens: 7 })
    },
  })
  const sse = await openSse(`${url}/inject`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ sessionId: 'session-x', text: 'hi there' }),
  })
  t.after(() => sse.close())

  assert.deepEqual(received, { sessionId: 'session-x', text: 'hi there', aborted: false })
  const first = await sse.next()
  assert.deepEqual(first, { id: undefined, event: 'token', data: { text: 'Hello ' } })
  const second = await sse.next()
  assert.deepEqual(second.data, { text: 'world' })
  const toolStart = await sse.next()
  assert.deepEqual(toolStart, { id: undefined, event: 'tool', data: { name: 'bash', phase: 'start', ok: true } })
  const toolEnd = await sse.next()
  assert.deepEqual(toolEnd.data, { name: 'bash', phase: 'end', ok: true })
  const done = await sse.next()
  assert.deepEqual(done, { id: undefined, event: 'done', data: { sessionId: 'session-x', tokens: 7 } })
})

test('POST /inject emits an error frame when the adapter throws', async (t) => {
  const { url } = await startTestServer(t, {
    inject: async () => {
      throw new Error('agent is shutting down')
    },
  })
  const sse = await openSse(`${url}/inject`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ sessionId: 'auto', text: 'hello' }),
  })
  t.after(() => sse.close())
  const frame = await sse.next()
  assert.equal(frame.event, 'error')
  assert.equal(frame.data.code, 'inject-failed')
  assert.match(frame.data.message, /agent is shutting down/)
})

test('POST /inject validates the prompt before opening the stream', async (t) => {
  const { url } = await startTestServer(t)
  for (const body of [{}, { sessionId: 'auto' }, { text: '' }, { text: '   ' }, { text: 42 }]) {
    const { status, body: response } = await postJson(`${url}/inject`, body)
    assert.equal(status, 400, JSON.stringify(body))
    assert.equal(response.error, 'invalid-request')
  }
})

test('POST /inject defaults a missing sessionId to auto', async (t) => {
  let received
  const { url } = await startTestServer(t, {
    inject: async ({ sessionId, emit }) => {
      received = sessionId
      emit({ type: 'done', sessionId: 'session-auto', tokens: 0 })
    },
  })
  const sse = await openSse(`${url}/inject`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ text: 'hello' }),
  })
  t.after(() => sse.close())
  await sse.next()
  assert.equal(received, 'auto')
})

test('an unknown session id becomes an SSE error frame', async (t) => {
  const session = createFakeSession('session-known')
  const ctx = createFakeCtx({ sessions: [session] })
  const agent = createFakeAgent(session)
  ctx.addAgent(agent)
  const { url } = await startTestBridge(t, ctx)
  const sse = await openSse(`${url}/inject`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ sessionId: 'session-missing', text: 'hello' }),
  })
  t.after(() => sse.close())
  const frame = await sse.next()
  assert.equal(frame.event, 'error')
  assert.equal(frame.data.code, 'session-not-found')
  assert.equal(agent.followups.length, 0)
})

test('a session without a live agent becomes an SSE error frame', async (t) => {
  const session = createFakeSession('session-cold')
  const ctx = createFakeCtx({ sessions: [session] })
  const { url } = await startTestBridge(t, ctx)
  const sse = await openSse(`${url}/inject`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ sessionId: 'session-cold', text: 'hello' }),
  })
  t.after(() => sse.close())
  const frame = await sse.next()
  assert.equal(frame.event, 'error')
  assert.equal(frame.data.code, 'agent-not-live')
})

test('the DSH adapter injects a user message and streams live reasoning output', async (t) => {
  const session = createFakeSession('session-live')
  const ctx = createFakeCtx({ sessions: [session] })
  const agent = createFakeAgent(session)
  ctx.addAgent(agent)
  const { bridge, url } = await startTestBridge(t, ctx)

  const sse = await openSse(`${url}/inject`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ sessionId: 'session-live', text: 'summarize the repo' }),
  })
  t.after(() => sse.close())

  await waitFor(() => agent.followups.length === 1)
  const message = agent.followups[0]
  assert.equal(message.role, 'user')
  assert.deepEqual(message.content, [{ type: 'text', text: 'summarize the repo' }])
  assert.deepEqual(message.source, { kind: 'user' })
  assert.match(message.id, /^[0-9a-f-]{36}$/)

  // Simulate the DSH loop publishing live activity for this agent/session.
  ctx.dispatchSync('agent/assistant-stream', {
    agent,
    frame: { type: 'chunk', chunk: { type: 'text-delta', text: 'The repo ' } },
  })
  ctx.dispatchSync('agent/assistant-stream', {
    agent,
    frame: { type: 'chunk', chunk: { type: 'text-delta', text: 'is small.' } },
  })
  ctx.dispatchSync('agent/assistant-stream', {
    agent,
    frame: { type: 'chunk', chunk: { type: 'usage', usage: { outputTokens: 999 } } },
  })
  // A different agent's stream must not leak into this injection.
  const other = createFakeAgent(createFakeSession('session-other'))
  ctx.dispatchSync('agent/assistant-stream', {
    agent: other,
    frame: { type: 'chunk', chunk: { type: 'text-delta', text: 'LEAK' } },
  })
  ctx.dispatchSync('session/event', session, { type: 'turn/start', time: Date.now(), data: { turn: 1 } })
  ctx.dispatchSync('session/event', session, {
    type: 'tool/call',
    time: Date.now(),
    data: { turn: 1, step: 1, callId: 'call-1', name: 'bash', arguments: '{}' },
  })
  ctx.dispatchSync('session/event', session, {
    type: 'tool/result',
    time: Date.now(),
    data: { turn: 1, step: 1, message: { callId: 'call-1', isError: false } },
  })
  ctx.dispatchSync('session/event', session, {
    type: 'assistant/message',
    time: Date.now(),
    data: { turn: 1, step: 1, message: {}, usage: { inputTokens: 10, outputTokens: 12 } },
  })
  agent.finish()

  const token1 = await sse.next()
  assert.deepEqual(token1, { id: undefined, event: 'token', data: { text: 'The repo ' } })
  const token2 = await sse.next()
  assert.deepEqual(token2.data, { text: 'is small.' })
  const toolStart = await sse.next()
  assert.deepEqual(toolStart.data, { name: 'bash', phase: 'start', ok: true })
  const toolEnd = await sse.next()
  assert.deepEqual(toolEnd.data, { name: 'bash', phase: 'end', ok: true })
  const done = await sse.next()
  assert.deepEqual(done, { id: undefined, event: 'done', data: { sessionId: 'session-live', tokens: 12 } })
})

test('auto selects the most recently observed live session', async (t) => {
  const older = createFakeSession('session-older', { createdAt: 1000 })
  const newer = createFakeSession('session-newer', { createdAt: 2000 })
  const olderAgent = createFakeAgent(older)
  const newerAgent = createFakeAgent(newer)
  const ctx = createFakeCtx({ sessions: [older, newer], agents: [olderAgent, newerAgent] })
  const { bridge, url } = await startTestBridge(t, ctx)
  // Make the older session the most recently active one.
  ctx.dispatchSync('session/event', older, { type: 'turn/start', time: 9000, data: { turn: 1 } })
  const target = bridge.resolveTargetSession('auto')
  assert.equal(target.id, 'session-older')
  void url
})

test('explicit session placement routes to that session even when another is newer', async (t) => {
  const one = createFakeSession('session-one', { createdAt: 1000 })
  const two = createFakeSession('session-two', { createdAt: 2000 })
  const oneAgent = createFakeAgent(one)
  const twoAgent = createFakeAgent(two)
  const ctx = createFakeCtx({ sessions: [one, two], agents: [oneAgent, twoAgent] })
  const { url } = await startTestBridge(t, ctx)
  const sse = await openSse(`${url}/inject`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ sessionId: 'session-one', text: 'write to one' }),
  })
  t.after(() => sse.close())
  await waitFor(() => oneAgent.followups.length === 1)
  assert.equal(twoAgent.followups.length, 0)
  oneAgent.finish()
  const done = await sse.next()
  assert.equal(done.event, 'done')
  assert.equal(done.data.sessionId, 'session-one')
})

test('inject times out with an error frame instead of hanging forever', async (t) => {
  const session = createFakeSession('session-hang')
  const ctx = createFakeCtx({ sessions: [session] })
  const agent = createFakeAgent(session)
  // Never resolves: simulates a wedged agent.
  agent.whenIdle = () => new Promise(() => {})
  ctx.addAgent(agent)
  const { bridge, url } = await startTestBridge(t, ctx, { injectTimeoutMs: 150 })
  const sse = await openSse(`${url}/inject`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ sessionId: 'session-hang', text: 'hello' }),
  })
  t.after(() => sse.close())
  const frame = await sse.next(3000)
  assert.equal(frame.event, 'error')
  assert.equal(frame.data.code, 'inject-failed')
  assert.match(frame.data.message, /inject wait exceeded/)
  void bridge
})

test('a non-object JSON body answers 400', async (t) => {
  const { url } = await startTestServer(t)
  for (const raw of ['[]', '"text"', '42', 'null']) {
    const { status } = await requestJson(`${url}/inject`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: raw,
    })
    assert.equal(status, 400, raw)
  }
})
