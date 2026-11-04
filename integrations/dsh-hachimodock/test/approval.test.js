'use strict'

/**
 * R5 approval contract: prompt broadcast, answerer outcomes, one-shot decisions.
 * Uses a fake Cordis context whose `approval/request` listener is the bridge's
 * real answerer, so this covers the DSH boundary without a live DSH process.
 */

const test = require('node:test')
const assert = require('node:assert/strict')

const {
  appendSessionEvent,
  nextControlEvent,
  createFakeCtx,
  createFakeSession,
  createApprovalRequest,
  startTestBridge,
  openSse,
  postJson,
  waitFor,
} = require('./helpers')

/**
 * Start a bridge plus one attached `/events` consumer.
 * @param {import('node:test').TestContext} t - test context.
 * @param {object} [config] - bridge config overrides.
 * @returns {Promise<{bridge: object, ctx: object, session: object, sse: object, url: string}>} fixtures.
 */
async function fixtures(t, config = {}) {
  const session = createFakeSession('session-approval')
  const ctx = createFakeCtx({ sessions: [session] })
  const { bridge, url } = await startTestBridge(t, ctx, config)
  const sse = await openSse(`${url}/events`)
  t.after(() => sse.close())
  await waitFor(() => bridge.server.hasEventSubscriber())
  return { bridge, ctx, session, sse, url }
}

/**
 * Invoke the registered answerer exactly like Cordis' waterfall would.
 * @param {object} ctx - fake context.
 * @param {object} request - approval request.
 * @param {Function} [next] - next answerer.
 * @returns {Promise<string>} answerer outcome.
 */
function answer(ctx, request, next = () => Promise.resolve('unavailable')) {
  return ctx.invokeFirst('approval/request', request, next)
}

test('an approval prompt is broadcast and allow resolves allowed-once', async (t) => {
  const { ctx, session, sse, url } = await fixtures(t)
  const request = createApprovalRequest(session)
  const outcome = answer(ctx, request)

  const frame = await sse.next()
  assert.equal(frame.event, 'approval')
  assert.equal(frame.data.type, 'approval')
  assert.equal(frame.data.seq, Number(frame.id.split(':').pop()))
  assert.equal(frame.data.sessionId, 'session-approval')
  assert.equal(frame.data.agentId, 'deepseek-harness')
  assert.equal(frame.data.tool, 'bash')
  assert.equal(frame.data.reason, 'run the test suite')
  assert.equal(frame.data.displayReason, 'Run tests?')
  assert.match(frame.data.requestId, /^req-/)
  assert.equal(Number.isSafeInteger(frame.data.expiresAtMs), true)
  assert.equal(frame.data.expiresAtMs > Date.now(), true)

  const decided = await postJson(`${url}/approval/decision`, {
    requestId: frame.data.requestId,
    decision: 'allow',
  })
  assert.equal(decided.status, 200)
  assert.deepEqual(decided.body, { ok: true, requestId: frame.data.requestId, accepted: true })
  assert.equal(await outcome, 'allowed-once')
})

test('deny resolves rejected', async (t) => {
  const { ctx, session, sse, url } = await fixtures(t)
  const outcome = answer(ctx, createApprovalRequest(session))
  const frame = await sse.next()
  const decided = await postJson(`${url}/approval/decision`, {
    requestId: frame.data.requestId,
    decision: 'deny',
  })
  assert.equal(decided.status, 200)
  assert.equal(await outcome, 'rejected')
})

test('an identical duplicate decision returns the original receipt', async (t) => {
  const { ctx, session, sse, url } = await fixtures(t)
  const outcome = answer(ctx, createApprovalRequest(session))
  const frame = await sse.next()
  const first = await postJson(`${url}/approval/decision`, {
    requestId: frame.data.requestId,
    decision: 'allow',
  })
  assert.equal(first.status, 200)
  await outcome

  const duplicate = await postJson(`${url}/approval/decision`, {
    requestId: frame.data.requestId,
    decision: 'allow',
  })
  assert.equal(duplicate.status, 200)
  assert.equal(duplicate.body.ok, true)
  assert.equal(duplicate.body.requestId, frame.data.requestId)
  assert.equal(duplicate.body.accepted, true)
})

test('an unknown requestId answers 409 unknown-or-expired', async (t) => {
  const { url } = await fixtures(t)
  const response = await postJson(`${url}/approval/decision`, {
    requestId: 'req-does-not-exist',
    decision: 'allow',
  })
  assert.equal(response.status, 409)
  assert.deepEqual(response.body, { ok: false, requestId: 'req-does-not-exist', error: 'unknown-or-expired' })
})

test('a decision after the timeout answers 409 unknown-or-expired', async (t) => {
  const { ctx, session, sse, url } = await fixtures(t, { approvalTimeoutMs: 120 })
  const startedAt = Date.now()
  const outcome = answer(ctx, createApprovalRequest(session))
  const frame = await sse.next()
  assert.equal(await outcome, 'unavailable')
  assert.equal(Date.now() - startedAt >= 100, true, 'timeout must actually wait')

  const late = await postJson(`${url}/approval/decision`, {
    requestId: frame.data.requestId,
    decision: 'allow',
  })
  assert.equal(late.status, 409)
  assert.equal(late.body.error, 'unknown-or-expired')
})

test('aborting the DSH request signal fails the approval closed promptly', async (t) => {
  const { ctx, session, sse } = await fixtures(t, { approvalTimeoutMs: 60000 })
  const controller = new AbortController()
  const outcome = answer(ctx, createApprovalRequest(session, { signal: controller.signal }))
  await sse.next()
  const startedAt = Date.now()
  controller.abort()
  assert.equal(await outcome, 'cancelled')
  assert.equal(Date.now() - startedAt < 1000, true)
})

test('no connected HachimoDock bridge answers unavailable immediately (fail closed)', async (t) => {
  const session = createFakeSession('session-offline')
  const ctx = createFakeCtx({ sessions: [session] })
  await startTestBridge(t, ctx, { approvalTimeoutMs: 60000 })
  const startedAt = Date.now()
  let delegated = false
  const outcome = await answer(ctx, createApprovalRequest(session), () => {
    delegated = true
    return Promise.resolve('allowed-once')
  })
  assert.equal(outcome, 'unavailable')
  assert.equal(delegated, false, 'default mode is terminal and fails closed')
  assert.equal(Date.now() - startedAt < 1000, true)
})

test('delegate fallback hands an unanswerable request to the next answerer', async (t) => {
  const session = createFakeSession('session-delegate')
  const ctx = createFakeCtx({ sessions: [session] })
  await startTestBridge(t, ctx, { approvalFallback: 'delegate' })
  let delegated = 0
  const outcome = await answer(ctx, createApprovalRequest(session), () => {
    delegated += 1
    return Promise.resolve('rejected')
  })
  assert.equal(outcome, 'rejected')
  assert.equal(delegated, 1)
})

test('two concurrent prompts keep independent request ids and decisions', async (t) => {
  const { ctx, session, sse, url } = await fixtures(t)
  const firstOutcome = answer(ctx, createApprovalRequest(session, { toolName: 'bash' }))
  const secondOutcome = answer(ctx, createApprovalRequest(session, { toolName: 'write_file' }))
  const first = await sse.next()
  const second = await sse.next()
  assert.notEqual(first.data.requestId, second.data.requestId)
  assert.equal(first.data.tool, 'bash')
  assert.equal(second.data.tool, 'write_file')

  await postJson(`${url}/approval/decision`, { requestId: second.data.requestId, decision: 'deny' })
  await postJson(`${url}/approval/decision`, { requestId: first.data.requestId, decision: 'allow' })
  assert.equal(await firstOutcome, 'allowed-once')
  assert.equal(await secondOutcome, 'rejected')
})

test('an invalid decision body answers 400', async (t) => {
  const { url } = await fixtures(t)
  for (const body of [{}, { requestId: 'req-1' }, { decision: 'allow' }, { requestId: 42, decision: 'allow' }]) {
    const { status } = await postJson(`${url}/approval/decision`, body)
    assert.equal(status, 400, JSON.stringify(body))
  }
})

test('the answerer never throws into the approval service', async (t) => {
  const session = createFakeSession('session-throwing')
  const ctx = createFakeCtx({ sessions: [session] })
  await startTestBridge(t, ctx)
  // No /events subscriber -> unavailable without touching the request fields.
  assert.equal(await answer(ctx, {}), 'unavailable')
  assert.equal(await answer(ctx, { agent: {} }), 'unavailable')
  assert.equal(await answer(ctx, null), 'unavailable')
})

test('official audit events are display-only and never guessed onto a request', async (t) => {
  const { bridge, ctx, session, sse, url } = await fixtures(t)
  appendSessionEvent(ctx, session, { type: 'turn/start', data: { turn: 1 } })
  appendSessionEvent(ctx, session, { type: 'tool/call', data: { turn: 1, callId: 'call-audit', name: 'bash' } })
  await nextControlEvent(sse, 'turn', session.id)
  await nextControlEvent(sse, 'tool', session.id)
  // DSH appends `approval/asked` synchronously before dispatching the answerer.
  appendSessionEvent(ctx, session, {
    type: 'approval/asked',
    time: Date.now(),
    data: { id: 'dsh-approval-1', callId: 'call-audit', toolName: 'bash', reason: 'run the test suite' },
  })
  const asked = await nextControlEvent(sse, 'approval-asked', session.id)
  assert.equal(asked.event, 'approval-asked')
  assert.equal(asked.data.approvalId, 'dsh-approval-1')
  assert.equal(bridge.server.pending.size, 0, 'display-only audit cannot authorize a tool')

  const outcome = answer(ctx, createApprovalRequest(session))
  const prompt = await nextControlEvent(sse, 'approval', session.id)
  assert.equal(prompt.event, 'approval')
  assert.equal(prompt.data.approvalId, undefined)

  await postJson(`${url}/approval/decision`, { requestId: prompt.data.requestId, decision: 'allow' })
  assert.equal(await outcome, 'allowed-once')

  appendSessionEvent(ctx, session, {
    type: 'approval/decided',
    time: Date.now(),
    data: { id: 'dsh-approval-1', outcome: 'allowed-once' },
  })
  const resolved = await nextControlEvent(sse, 'approval-resolved', session.id)
  assert.equal(resolved.event, 'approval-resolved')
  assert.equal(resolved.data.approvalId, 'dsh-approval-1')
  assert.equal(resolved.data.requestId, undefined)
  assert.equal(resolved.data.outcome, 'allowed-once')
})

test('a finite timeout emits approval-expired after the prompt', async (t) => {
  const { ctx, session, sse } = await fixtures(t, { approvalTimeoutMs: 120 })
  const outcome = answer(ctx, createApprovalRequest(session))
  const prompt = await sse.next()
  const expired = await sse.next()
  assert.equal(expired.event, 'approval-expired')
  assert.equal(expired.data.requestId, prompt.data.requestId)
  assert.equal(await outcome, 'unavailable')
})

test('the large never sentinel never expires and still accepts a decision', async (t) => {
  const NEVER = 99999
  const { ctx, session, sse, url } = await fixtures(t, { approvalTimeoutMs: NEVER })
  const outcome = answer(ctx, createApprovalRequest(session))
  const prompt = await sse.next()
  assert.equal(prompt.data.neverExpires, true)
  assert.equal(prompt.data.expiresAtMs, 0)
  await new Promise((resolve) => setTimeout(resolve, 180))
  const decided = await postJson(`${url}/approval/decision`, {
    requestId: prompt.data.requestId,
    decision: 'allow',
  })
  assert.equal(decided.status, 200)
  assert.equal(await outcome, 'allowed-once')
})

test('pinned official localized displayReason object preserves presentation without audit ID guessing', async t => {
  const { ctx, session, sse, url } = await fixtures(t)
  const outcome = answer(ctx, createApprovalRequest(session, { displayReason: { en: 'Run tests?', 'zh-CN': '运行测试？' } }))
  const frame = await sse.next()
  assert.equal(frame.data.displayReason, '运行测试？')
  await postJson(`${url}/approval/decision`, { requestId: frame.data.requestId, decision: 'deny' })
  assert.equal(await outcome, 'rejected')
})
