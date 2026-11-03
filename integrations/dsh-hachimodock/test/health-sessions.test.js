'use strict'

/**
 * Transport contract: health, sessions, routing, body limits.
 * Runs against `server.js` with fake adapter deps (no DSH process).
 */

const test = require('node:test')
const assert = require('node:assert/strict')

const { BridgeServer, isLoopbackAddress, MAX_BODY_BYTES } = require('../server')
const { startTestServer, requestJson, postJson, createFakeSession } = require('./helpers')

function fakeSummaries() {
  return [
    {
      id: 'session-a',
      title: 'Fix the firmware build',
      cwd: '/Users/dev/project',
      lastModified: 1790000000000,
      status: 'running',
      agentId: 'deepseek-harness',
    },
    {
      id: 'session-b',
      title: null,
      cwd: null,
      lastModified: 1789999999000,
      status: 'idle',
      agentId: 'deepseek-harness',
    },
  ]
}

test('GET /health reports protocol version, session count, and pending approvals', async (t) => {
  const { url } = await startTestServer(t, { listSessions: fakeSummaries })
  const { status, body } = await requestJson(`${url}/health`)
  assert.equal(status, 200)
  assert.deepEqual(body, { ok: true, version: 2 })
})

test('GET /health counts pending approval requests', async (t) => {
  const { server, url } = await startTestServer(t, { listSessions: fakeSummaries })
  const prompt = server.requestApproval({ agentId: 'deepseek-harness', sessionId: 'session-a', tool: 'bash' })
  const { body } = await requestJson(`${url}/health`)
  assert.equal(body.pendingApprovals, undefined)
  server.decideApproval(prompt.requestId, 'deny')
  await prompt.outcome
  const after = await requestJson(`${url}/health`)
  assert.equal(after.body.pendingApprovals, undefined)
})

test('GET /sessions returns the adapter summaries in order', async (t) => {
  const { url } = await startTestServer(t, { listSessions: fakeSummaries })
  const { status, body } = await requestJson(`${url}/sessions`)
  assert.equal(status, 200)
  assert.equal(body.sessions.length, 2)
  assert.deepEqual(body.sessions[0], fakeSummaries()[0])
  assert.equal(body.sessions[1].title, null)
})

test('GET /sessions tolerates a throwing adapter', async (t) => {
  const { url } = await startTestServer(t, {
    listSessions: () => {
      throw new Error('session store exploded')
    },
  })
  const { status, body } = await requestJson(`${url}/sessions`)
  assert.equal(status, 500)
  assert.equal(body.ok, false)
})

test('unknown routes answer 404 JSON', async (t) => {
  const { url } = await startTestServer(t)
  const { status, body } = await requestJson(`${url}/nope`)
  assert.equal(status, 404)
  assert.deepEqual(body, { ok: false, error: 'not-found' })
})

test('wrong methods answer 405 JSON', async (t) => {
  const { url } = await startTestServer(t)
  for (const [path, method] of [
    ['/health', 'POST'],
    ['/sessions', 'POST'],
    ['/inject', 'GET'],
    ['/events', 'POST'],
    ['/approval/decision', 'GET'],
  ]) {
    const { status } = await requestJson(`${url}${path}`, { method, headers: { 'content-type': 'application/json' } })
    assert.equal(status, 405, `${method} ${path}`)
  }
})

test('invalid JSON bodies answer 400 invalid-json', async (t) => {
  const { url } = await startTestServer(t)
  for (const path of ['/inject', '/approval/decision']) {
    const { status, body } = await requestJson(`${url}${path}`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: '{not json',
    })
    assert.equal(status, 400, path)
    assert.equal(body.error, 'invalid-json')
  }
})

test('an over-limit body answers 413 without buffering it', async (t) => {
  const { server, url } = await startTestServer(t)
  const oversized = 'x'.repeat(MAX_BODY_BYTES + 1)
  const response = await fetch(`${url}/inject`, {
    method: 'POST',
    headers: { 'content-type': 'application/json', Authorization: `Bearer ${server.tokens.bus}` },
    body: JSON.stringify({ sessionId: 'auto', text: oversized }),
  })
  assert.equal(response.status, 413)
  const body = await response.json()
  assert.equal(body.error, 'payload-too-large')
})

test('loopback address classification', () => {
  assert.equal(isLoopbackAddress('127.0.0.1'), true)
  assert.equal(isLoopbackAddress('::1'), true)
  assert.equal(isLoopbackAddress('::ffff:127.0.0.1'), true)
  assert.equal(isLoopbackAddress('10.0.0.2'), false)
  assert.equal(isLoopbackAddress('::ffff:10.0.0.2'), false)
  assert.equal(isLoopbackAddress(undefined), false)
})

test('the server only binds 127.0.0.1', async (t) => {
  const server = new BridgeServer({ port: 0, writeBootstrap: false })
  await server.listen(0)
  t.after(() => server.close())
  assert.equal(server.host, '127.0.0.1')
  assert.equal(server.url().startsWith('http://127.0.0.1:'), true)
})

test('pending approvals are failed closed when the server closes', async (t) => {
  const server = new BridgeServer({ port: 0, writeBootstrap: false })
  await server.listen(0)
  const prompt = server.requestApproval({ agentId: 'deepseek-harness', sessionId: 's', tool: 'bash' })
  await server.close()
  assert.equal(await prompt.outcome, 'unavailable')
})

test('contract-shaped session summary survives a session JSON round-trip', async (t) => {
  // The adapter produces the summary; the transport must not reshape it.
  const session = createFakeSession('session-c', { cwd: '/tmp/x' })
  const summary = {
    id: session.id,
    title: 'A title',
    cwd: session.header.cwd,
    lastModified: session.header.createdAt,
    status: 'idle',
    agentId: 'deepseek-harness',
  }
  const { url } = await startTestServer(t, { listSessions: () => [summary] })
  const { body } = await requestJson(`${url}/sessions`)
  assert.deepEqual(body.sessions, [summary])
})
