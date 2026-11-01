'use strict'

/**
 * GET /events SSE contract: cursor replay, live forwarding, compact session
 * event translation (turn/tool/title/session), and payload bounds.
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
  assert.equal(frame.id, '2')
})

test('events?after=<seq> replays the retained window after the cursor', async (t) => {
  const { server, url } = await startTestServer(t)
  server.publish('turn', { sessionId: 's1', phase: 'start' })
  server.publish('tool', { sessionId: 's1', phase: 'start', name: 'bash' })
  server.publish('session-title', { sessionId: 's1', title: 'A title' })
  const sse = await openSse(`${url}/events?after=1`)
  t.after(() => sse.close())
  const first = await sse.next()
  assert.equal(first.id, '2')
  assert.equal(first.event, 'tool')
  const second = await sse.next()
  assert.equal(second.id, '3')
  assert.equal(second.event, 'session-title')
  assert.equal(second.data.title, 'A title')
})

test('events?after rejects a non-numeric cursor with 400', async (t) => {
  const { url } = await startTestServer(t)
  for (const after of ['abc', '-1', '1.5']) {
    const { status, body } = await requestJson(`${url}/events?after=${after}`)
    assert.equal(status, 400, after)
    assert.equal(body.error, 'invalid-after')
  }
})

test('session/event forwarding emits compact turn, tool, title and approval events', async (t) => {
  const session = createFakeSession('session-events', { cwd: '/tmp/events' })
  const ctx = createFakeCtx({ sessions: [session] })
  const { bridge, url } = await startTestBridge(t, ctx)
  const sse = await openSse(`${url}/events?after=0`)
  t.after(() => sse.close())
  await waitFor(() => bridge.server.hasEventSubscriber())

  ctx.dispatchSync('session/event', session, { type: 'turn/start', time: 1000, data: { turn: 1 } })
  ctx.dispatchSync('session/event', session, {
    type: 'tool/call',
    time: 1001,
    data: { turn: 1, step: 1, callId: 'call-1', name: 'bash', arguments: '{"cmd":"ls"}' },
  })
  ctx.dispatchSync('session/event', session, {
    type: 'tool/result',
    time: 1002,
    data: { turn: 1, step: 1, message: { callId: 'call-1', isError: true } },
  })
  ctx.dispatchSync('session/event', session, {
    type: 'session/title',
    time: 1003,
    data: { title: 'Refactor the bridge', messageSeqs: [0], source: { kind: 'user' } },
  })
  ctx.dispatchSync('session/event', session, {
    type: 'approval/asked',
    time: 1004,
    data: { id: 'approval-1', toolName: 'bash', reason: 'go' },
  })
  ctx.dispatchSync('session/event', session, {
    type: 'approval/decided',
    time: 1005,
    data: { id: 'approval-1', outcome: 'rejected' },
  })
  ctx.dispatchSync('session/event', session, { type: 'turn/end', time: 1006, data: { turn: 1 } })

  const turnStart = await sse.next()
  assert.equal(turnStart.event, 'turn')
  assert.equal(turnStart.data.phase, 'start')
  assert.equal(turnStart.data.sessionId, 'session-events')
  const toolStart = await sse.next()
  assert.equal(toolStart.event, 'tool')
  assert.deepEqual(
    {
      sessionId: toolStart.data.sessionId,
      phase: toolStart.data.phase,
      name: toolStart.data.name,
      callId: toolStart.data.callId,
      ok: toolStart.data.ok,
    },
    { sessionId: 'session-events', phase: 'start', name: 'bash', callId: 'call-1', ok: true },
  )
  // The event must not mirror tool arguments.
  assert.equal('arguments' in toolStart.data, false)
  const toolEnd = await sse.next()
  assert.equal(toolEnd.data.phase, 'end')
  assert.equal(toolEnd.data.ok, false)
  const title = await sse.next()
  assert.equal(title.event, 'session-title')
  assert.equal(title.data.sessionId, 'session-events')
  assert.equal(title.data.title, 'Refactor the bridge')
  const asked = await sse.next()
  assert.equal(asked.event, 'approval-asked')
  assert.equal(asked.data.approvalId, 'approval-1')
  assert.equal(asked.data.tool, 'bash')
  const resolved = await sse.next()
  assert.equal(resolved.event, 'approval-resolved')
  assert.equal(resolved.data.outcome, 'rejected')
  const turnEnd = await sse.next()
  assert.equal(turnEnd.data.phase, 'end')
})

test('title/session summaries expose tracked state and stay bounded', async (t) => {
  const session = createFakeSession('session-summary', { cwd: '/tmp/summary' })
  const ctx = createFakeCtx({ sessions: [session] })
  const agent = createFakeAgent(session)
  agent.status = 'running'
  ctx.addAgent(agent)
  const { bridge, url } = await startTestBridge(t, ctx)
  const sse = await openSse(`${url}/events?after=0`)
  t.after(() => sse.close())
  await waitFor(() => bridge.server.hasEventSubscriber())

  const longTitle = 'x'.repeat(400)
  ctx.dispatchSync('session/event', session, {
    type: 'session/title',
    time: 4242,
    data: { title: longTitle, messageSeqs: [], source: { kind: 'user' } },
  })
  const titleFrame = await sse.next()
  assert.equal(titleFrame.data.title.length <= 160, true)

  const { body } = await requestJson(`${url}/sessions`)
  assert.equal(body.sessions.length, 1)
  assert.equal(body.sessions[0].id, 'session-summary')
  assert.equal(body.sessions[0].cwd, '/tmp/summary')
  assert.equal(body.sessions[0].status, 'running')
  assert.equal(body.sessions[0].agentId, 'deepseek-harness')
  assert.equal(body.sessions[0].lastModified, 4242)
  assert.equal(body.sessions[0].title.length <= 160, true)

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
  const sse = await openSse(`${url}/events?after=0`)
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
  const sse = await openSse(`${url}/events?after=0`)
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

  ctx.dispatchSync('session/event', session, { type: 'turn/start', time: 2000, data: { turn: 1 } })
  ctx.dispatchSync('session/event', session, {
    type: 'tool/call',
    time: 2001,
    data: { turn: 1, step: 1, callId: 'call-state', name: 'bash', arguments: '{"cmd":"sleep 1"}' },
  })
  let { body } = await requestJson(`${url}/sessions`)
  assert.equal(body.sessions[0].state, 'tool_running')
  const toolRevision = body.sessions[0].transitionRevision

  ctx.dispatchSync('session/event', session, {
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
  ctx.dispatchSync('session/event', session, {
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

test('stopped DSH turns map to idle instead of a completed state', async (t) => {
  const session = createFakeSession('session-stopped')
  const ctx = createFakeCtx({ sessions: [session] })
  const { url } = await startTestBridge(t, ctx)

  ctx.dispatchSync('session/event', session, {
    type: 'turn/start',
    time: 7000,
    data: { turn: 1 },
  })
  ctx.dispatchSync('session/event', session, {
    type: 'turn/end',
    time: 7001,
    data: { turn: 1, reason: { kind: 'aborted', reason: 'user' } },
  })
  let { body } = await requestJson(`${url}/sessions`)
  assert.equal(body.sessions[0].state, 'stopped')

  ctx.dispatchSync('session/event', session, {
    type: 'turn/start',
    time: 7002,
    data: { turn: 2 },
  })
  ctx.dispatchSync('session/event', session, {
    type: 'turn/end',
    time: 7003,
    data: { turn: 2, reason: { kind: 'completed' } },
  })
  ;({ body } = await requestJson(`${url}/sessions`))
  assert.equal(body.sessions[0].state, 'done')
})
