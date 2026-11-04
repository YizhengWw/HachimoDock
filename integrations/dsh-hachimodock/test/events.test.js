'use strict'

/**
 * GET /events SSE contract: cursor replay, live forwarding, compact session
 * event translation (turn/tool/title/session), and payload bounds.
 */

const test = require('node:test')
const assert = require('node:assert/strict')

const {
  appendSessionEvent,
  nextControlEvent,
  createFakeCtx,
  createFakeSession,
  createFakeAgent,
  startTestServer,
  startTestBridge,
  openSse,
  requestJson,
  waitFor,
} = require('./helpers')

test('events without an after cursor only stream new events', async (t) => {
  const { server, url } = await startTestServer(t)
  server.publish('turn', { sessionId: 'before', phase: 'start' })
  const sse = await openSse(`${url}/events`)
  t.after(() => sse.close())
  await waitFor(() => server.hasEventSubscriber())
  server.publish('turn', { sessionId: 'after', phase: 'start' })
  const frame = await sse.next()
  assert.equal(frame.event, 'turn')
  assert.equal(frame.data.sessionId, 'after')
  assert.equal(frame.data.seq, 2)
  assert.equal(frame.id, `${server.epoch}:native-relay:2`)
})

test('events?after=<seq> replays the retained window after the cursor', async (t) => {
  const { server, url } = await startTestServer(t)
  server.publish('turn', { sessionId: 's1', phase: 'start' })
  server.publish('tool', { sessionId: 's1', phase: 'start', name: 'bash' })
  server.publish('session-title', { sessionId: 's1', title: 'A title' })
  const sse = await openSse(`${url}/events?after=${server.epoch}:native-relay:1`)
  t.after(() => sse.close())
  const first = await sse.next()
  assert.equal(first.id, `${server.epoch}:native-relay:2`)
  assert.equal(first.event, 'tool')
  const second = await sse.next()
  assert.equal(second.id, `${server.epoch}:native-relay:3`)
  assert.equal(second.event, 'session-title')
  assert.equal(second.data.title, 'A title')
})

test('events?after rejects a non-numeric cursor with 400', async (t) => {
  const { server, url } = await startTestServer(t)
  for (const after of ['abc', '-1', '1.5']) {
    const { status, body } = await requestJson(`${url}/events?after=${server.epoch}:native-relay:${after}`)
    assert.equal(status, 400, after)
    assert.equal(body.error, 'invalid-after')
  }
})

test('session/event forwarding emits compact turn, tool, title and approval events', async t => {
  const session = createFakeSession('session-events', { cwd: '/tmp/events' })
  const ctx = createFakeCtx({ sessions: [session] })
  const { bridge, url } = await startTestBridge(t, ctx)
  const sse = await openSse(`${url}/events?after=${bridge.server.epoch}:native-relay:0`)
  t.after(() => sse.close())
  await waitFor(() => bridge.server.hasEventSubscriber())
  appendSessionEvent(ctx, session, { type: 'turn/start', time: 1000, data: { turn: 1 } })
  appendSessionEvent(ctx, session, { type: 'tool/call', time: 1001,
    data: { turn: 1, step: 1, callId: 'call-1', name: 'bash', arguments: '{"cmd":"ls"}' } })
  // Official asked audit can omit turn, but must identify the still-current call.
  appendSessionEvent(ctx, session, { type: 'approval/asked', time: 1002,
    data: { id: 'approval-1', callId: 'call-1', toolName: 'bash', reason: 'go' } })
  appendSessionEvent(ctx, session, { type: 'approval/decided', time: 1003,
    data: { id: 'approval-1', outcome: 'rejected' } })
  appendSessionEvent(ctx, session, { type: 'tool/result', time: 1004,
    data: { turn: 1, step: 1, message: { toolCallId: 'call-1', isError: true } } })
  appendSessionEvent(ctx, session, { type: 'session/title', time: 1005,
    data: { title: 'Refactor the bridge', messageSeqs: [0], source: { kind: 'user' } } })
  appendSessionEvent(ctx, session, { type: 'turn/end', time: 1006,
    data: { turn: 1, reason: { kind: 'completed' } } })
  const next = kind => nextControlEvent(sse, kind, session.id)
  const start = await next('turn'); assert.equal(start.data.phase, 'start'); assert.equal(start.data.state, 'working')
  const call = await next('tool'); assert.equal(call.data.name, 'bash'); assert.equal(call.data.callId, 'call-1')
  assert.equal(call.data.ok, true); assert.equal('arguments' in call.data, false)
  const asked = await next('approval-asked'); assert.equal(asked.data.approvalId, 'approval-1')
  assert.equal(asked.data.state, 'waiting_user'); assert.equal(asked.data.waitKind, 'toolapproval')
  const decided = await next('approval-resolved'); assert.equal(decided.data.outcome, 'rejected')
  const result = await next('tool'); assert.equal(result.data.phase, 'end'); assert.equal(result.data.ok, false)
  const title = await next('session-title'); assert.equal(title.data.title, 'Refactor the bridge')
  const end = await next('turn'); assert.equal(end.data.phase, 'end'); assert.equal(end.data.state, 'done')
  assert.equal(bridge.server.pending.size, 0, 'audit notifications never create actionable approval requests')
})

test('title/session summaries expose tracked state and stay bounded', async (t) => {
  const session = createFakeSession('session-summary', { cwd: '/tmp/summary' })
  const ctx = createFakeCtx({ sessions: [session] })
  const agent = createFakeAgent(session)
  agent.status = 'running'
  ctx.addAgent(agent)
  const { bridge, url } = await startTestBridge(t, ctx)
  const sse = await openSse(`${url}/events?after=${bridge.server.epoch}:native-relay:0`)
  t.after(() => sse.close())
  await waitFor(() => bridge.server.hasEventSubscriber())

  const longTitle = 'x'.repeat(400)
  appendSessionEvent(ctx, session, {
    type: 'session/title',
    time: 4242,
    data: { title: longTitle, messageSeqs: [], source: { kind: 'user' } },
  })
  const titleFrame = await nextControlEvent(sse, 'session-title', session.id)
  assert.equal(titleFrame.data.title.length <= 160, true)

  const { body } = await requestJson(`${url}/sessions`)
  assert.equal(body.sessions.length, 1)
  assert.equal(body.sessions[0].id, 'session-summary')
  assert.equal(body.sessions[0].cwd, '/tmp/summary')
  assert.equal(body.sessions[0].status, 'running')
  assert.equal(body.sessions[0].agentId, 'deepseek-harness')
  assert.equal(body.sessions[0].lastModified, 4242)
  assert.equal(body.sessions[0].title.length <= 160, true)

  const afterTitle = await sse.next()
  assert.equal(afterTitle.event, 'session')
  assert.equal(afterTitle.data.session.status, 'running')

  // agent/status transitions publish a fresh session summary.
  agent.status = 'idle'
  ctx.dispatchSync('agent/status', { status: 'idle', agent })
  const summaryFrame = await sse.next()
  assert.equal(summaryFrame.event, 'session')
  assert.equal(summaryFrame.data.session.status, 'idle')
})

test('session/created publishes a summary and session/disposed publishes removal', async (t) => {
  const session = createFakeSession('session-lifecycle')
  const ctx = createFakeCtx({ sessions: [session] })
  const { bridge, url } = await startTestBridge(t, ctx)
  const sse = await openSse(`${url}/events?after=${bridge.server.epoch}:native-relay:0`)
  t.after(() => sse.close())
  await waitFor(() => bridge.server.hasEventSubscriber())
  ctx.dispatchSync('session/created', session)
  const created = await sse.next()
  assert.equal(created.event, 'session')
  assert.equal(created.data.session.id, 'session-lifecycle')
  ctx.dispatchSync('session/disposed', session)
  const disposed = await sse.next()
  assert.equal(disposed.event, 'session')
  assert.equal(disposed.data.session.removed, true)
})

test('sessions with no title read the optional sessionTitle projection', async (t) => {
  const session = createFakeSession('session-title-service')
  const ctx = createFakeCtx({
    sessions: [session],
    services: {
      sessionTitle: {
        get: (target) => (target === session ? { title: 'from service' } : undefined),
      },
    },
  })
  const { url } = await startTestBridge(t, ctx)
  const { body } = await requestJson(`${url}/sessions`)
  assert.equal(body.sessions[0].title, 'from service')
})

test('the replay buffer is bounded', async (t) => {
  const { server, url } = await startTestServer(t)
  for (let index = 0; index < 600; index += 1) server.publish('turn', { index })
  const sse = await openSse(`${url}/events?after=${server.epoch}:native-relay:0`)
  t.after(() => sse.close())
  // Only the retained tail is replayed; the newest event is always present.
  const replay = server.events
  assert.equal(replay.length <= 512, true)
  assert.equal(replay[replay.length - 1].index, 599)
  const first = await sse.next()
  assert.equal(first.data.index, 600 - replay.length)
})

test('tool/call publishes tool_running and tool/result returns to working', async (t) => {
  const session = createFakeSession('session-tool-state', { cwd: '/tmp/tool-state' })
  const ctx = createFakeCtx({ sessions: [session] })
  const { bridge, url } = await startTestBridge(t, ctx)
  await waitFor(() => bridge.server.hasEventSubscriber() === false)

  appendSessionEvent(ctx, session, { type: 'turn/start', time: 2000, data: { turn: 1 } })
  appendSessionEvent(ctx, session, {
    type: 'tool/call',
    time: 2001,
    data: { turn: 1, step: 1, callId: 'call-state', name: 'bash', arguments: '{"cmd":"sleep 1"}' },
  })
  let { body } = await requestJson(`${url}/sessions`)
  assert.equal(body.sessions[0].state, 'tool_running')
  const toolRevision = body.sessions[0].transitionRevision

  appendSessionEvent(ctx, session, {
    type: 'tool/result',
    time: 2002,
    data: { turn: 1, step: 1, message: { callId: 'call-state' } },
  })
  ;({ body } = await requestJson(`${url}/sessions`))
  assert.equal(body.sessions[0].state, 'working')
  assert.ok(body.sessions[0].transitionRevision > toolRevision)
})

test('assistant text is exposed as bounded displayContent for the P4 body', async (t) => {
  const session = createFakeSession('session-assistant-text', { cwd: '/tmp/text' })
  const ctx = createFakeCtx({ sessions: [session] })
  const { url } = await startTestBridge(t, ctx)
  appendSessionEvent(ctx, session, {
    type: 'assistant/message',
    time: 5000,
    data: {
      turn: 1,
      step: 1,
      message: {
        role: 'assistant',
        content: [
          { type: 'reasoning', text: 'hidden reasoning' },
          { type: 'text', text: '### 标题\n**粗体**\n- 列表' },
          { type: 'tool-call', id: 'call-text', name: 'bash', arguments: '{}' },
        ],
      },
    },
  })
  const { body } = await requestJson(`${url}/sessions`)
  assert.equal(body.sessions[0].displayContent, '### 标题\n**粗体**\n- 列表')
  assert.equal(body.sessions[0].displayContent.includes('hidden reasoning'), false)
})

test('stopped DSH turns remain distinct from completed state', async (t) => {
  const session = createFakeSession('session-stopped')
  const ctx = createFakeCtx({ sessions: [session] })
  const { url } = await startTestBridge(t, ctx)

  appendSessionEvent(ctx, session, {
    type: 'turn/start',
    time: 7000,
    data: { turn: 1 },
  })
  appendSessionEvent(ctx, session, {
    type: 'turn/end',
    time: 7001,
    data: { turn: 1, reason: { kind: 'aborted', reason: 'user' } },
  })
  let { body } = await requestJson(`${url}/sessions`)
  assert.equal(body.sessions[0].state, 'stopped')

  appendSessionEvent(ctx, session, {
    type: 'turn/start',
    time: 7002,
    data: { turn: 2 },
  })
  appendSessionEvent(ctx, session, {
    type: 'turn/end',
    time: 7003,
    data: { turn: 2, reason: { kind: 'completed' } },
  })
  ;({ body } = await requestJson(`${url}/sessions`))
  assert.equal(body.sessions[0].state, 'done')
})


test('live state rejects duplicate sequence, old turn and gaps without guessing completion', async t => {
  const session = createFakeSession('live-negative'), ctx = createFakeCtx({ sessions: [session] })
  const { bridge } = await startTestBridge(t, ctx)
  const emit = (type, data) => appendSessionEvent(ctx, session, { type, data })
  const state = () => bridge.summarizeSession(session).state
  emit('turn/start', { turn: 1 }); assert.equal(state(), 'working')
  ctx.dispatchSync('session/event', session, { seq: 0, type: 'turn/end', data: { turn: 1, reason: { kind: 'completed' } } })
  assert.equal(state(), 'working')
  emit('tool/call', { turn: 0, callId: 'old', name: 'bash' }); assert.equal(state(), 'working')
  emit('tool/call', { turn: 1, callId: 'current', name: 'bash' }); assert.equal(state(), 'tool_running')
  session.seq += 2
  ctx.dispatchSync('session/event', session, { seq: session.seq - 1, type: 'assistant/message', data: {} })
  assert.equal(state(), 'unknown')
})

test('missing event fields and uncorrelated approval audits stay unknown and cannot grant', async t => {
  const session = createFakeSession('unsupported-audit'), ctx = createFakeCtx({ sessions: [session] })
  const { bridge } = await startTestBridge(t, ctx)
  ctx.dispatchSync('session/event', session, { type: 'turn/start', data: { turn: 1 } })
  assert.equal(bridge.summarizeSession(session).state, 'unknown')
  appendSessionEvent(ctx, session, { type: 'turn/start', data: { turn: 1 } })
  appendSessionEvent(ctx, session, { type: 'approval/asked', data: { id: 'unbound', toolName: 'bash' } })
  assert.equal(bridge.summarizeSession(session).state, 'unknown')
  assert.equal(bridge.server.pending.size, 0)
})

test('question continuation resumes only on admitted exact call and unknown end reason never becomes done', async t => {
  const session = createFakeSession('question-resume'), ctx = createFakeCtx({ sessions: [session] })
  const { bridge } = await startTestBridge(t, ctx)
  const emit = (type, data) => appendSessionEvent(ctx, session, { type, data })
  const state = () => bridge.summarizeSession(session)
  emit('turn/start', { turn: 1 })
  emit('tool/call', { turn: 1, callId: 'q', name: 'ask_user_question' })
  assert.equal(state().state, 'waiting_user'); assert.equal(state().waitKind, 'question')
  emit('tool/result', { turn: 1, message: { toolCallId: 'wrong', content: [{ type: 'text', text: '{"answers":[]}' }] } })
  assert.equal(state().state, 'waiting_user')
  emit('tool/result', { turn: 1, message: { toolCallId: 'q', content: [{ type: 'text', text: '{"pending":true}' }] } })
  emit('turn/end', { turn: 1, reason: { kind: 'completed' } })
  assert.equal(state().state, 'waiting_user')
  emit('user/message', { source: { kind: 'user-question-reply', callId: 'wrong' } })
  assert.equal(state().state, 'waiting_user')
  emit('user/message', { source: { kind: 'user-question-reply', callId: 'q' } })
  assert.equal(state().state, 'unknown', 'admission before next turn is not proof of active work')
  emit('turn/start', { turn: 2 }); assert.equal(state().state, 'working')
  emit('approval/asked', { turn: 1, id: 'late', toolName: 'bash' }); assert.equal(state().state, 'working')
  emit('turn/end', { turn: 2, reason: { kind: 'future-unknown' } }); assert.equal(state().state, 'unknown')
})
