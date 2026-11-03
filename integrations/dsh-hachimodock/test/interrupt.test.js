'use strict'
const test = require('node:test'), assert = require('node:assert/strict')
const { createFakeCtx, createFakeSession, startTestBridge, postJson } = require('./helpers')
test('interrupt uses official exact-session cancel and preserves its admission semantics', async t => {
  const ctx = createFakeCtx({ sessions: [createFakeSession('a'), createFakeSession('b')] }); const cancelled = []
  ctx.sessionController = { cancel: r => { cancelled.push(r); return { accepted: true } } }
  const { url } = await startTestBridge(t, ctx)
  const r = await postJson(url + '/interrupt', { sessionId: 'a' })
  assert.equal(r.status, 200); assert.equal(r.body.state, 'cancel-requested')
  assert.deepEqual(cancelled, [{ sessionId: 'a' }])
  assert.equal((await postJson(url + '/interrupt', { sessionId: 'auto' })).status, 400)
  assert.equal(cancelled.length, 1)
})
test('missing official cancel service does not fall back to agent.cancel', async t => {
  const ctx = createFakeCtx({ sessions: [createFakeSession('a')] })
  const { url } = await startTestBridge(t, ctx)
  assert.equal((await postJson(url + '/interrupt', { sessionId: 'a' })).status, 409)
})
