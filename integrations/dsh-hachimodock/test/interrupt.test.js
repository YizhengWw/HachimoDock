'use strict'

/** POST /interrupt cancels a live running DSH agent and is a no-op when idle. */

const test = require('node:test')
const assert = require('node:assert/strict')

const {
  createFakeCtx,
  createFakeSession,
  createFakeAgent,
  startTestBridge,
  postJson,
} = require('./helpers')

test('interrupt cancels the running DSH agent', async (t) => {
  const session = createFakeSession('session-interrupt')
  const agent = createFakeAgent(session)
  agent.status = 'running'
  const ctx = createFakeCtx({ sessions: [session], agents: [agent] })
  const { url } = await startTestBridge(t, ctx)
  const response = await postJson(`${url}/interrupt`, { sessionId: session.id })
  assert.equal(response.status, 200)
  assert.equal(response.body.ok, true)
  assert.equal(response.body.interrupted, true)
  assert.deepEqual(agent.cancelled, { kind: 'user' })
})

test('interrupting an idle agent reports no active turn', async (t) => {
  const session = createFakeSession('session-idle')
  const agent = createFakeAgent(session)
  const ctx = createFakeCtx({ sessions: [session], agents: [agent] })
  const { url } = await startTestBridge(t, ctx)
  const response = await postJson(`${url}/interrupt`, { sessionId: session.id })
  assert.equal(response.status, 200)
  assert.equal(response.body.ok, true)
  assert.equal(response.body.interrupted, false)
  assert.equal(agent.cancelled, null)
})
