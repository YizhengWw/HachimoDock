'use strict'
const test = require('node:test'), assert = require('node:assert/strict')
const { createVoiceConsumer } = require('../client')
const { startTestServer, openSse, waitFor } = require('./helpers')
const origin = 'https://dsh.example.test:7443'
function fakeInput(initial = '') {
  let rev = 1, submits = 0, inserts = 0
  const state = { phase: 'plain', draft: initial, draftRev: rev, attachmentIds: [], occurrences: [] }
  return { state, get submits() { return submits }, get inserts() { return inserts }, actions: {
    captureInsertion: () => ({ start: state.draft.length, end: state.draft.length, draftRev: rev }),
    insertText(text, span) { if (span.draftRev !== rev || state.phase !== 'plain') return false; inserts++; state.draft += text; state.draftRev = ++rev; return true },
    submit() { submits++ },
  }, edit(text) { state.draft = text; state.draftRev = ++rev } }
}
function consumer(input = fakeInput(), sessionId = 'a', clientId = 'c') {
  const receipts = [], c = createVoiceConsumer({ sessionId, clientId, epoch: 'epoch', inputActions: input.actions, readInput: () => input.state, ack: async p => receipts.push(p) })
  let seq = 0
  const event = (extra = {}) => ({ sessionId, owner: clientId, epoch: 'epoch', lease: 'lease', utteranceId: 'u', actionId: `action-${++seq}`, revision: 1, ...extra })
  return { input, receipts, c, event }
}
test('typed scoped voice captures before ASR and inserts/submits once with precise revision', async () => {
  const { input, receipts, c, event } = consumer()
  const begin = event(), insert = event({ text: 'voice' }), submit = event()
  await c.handle('voice-begin', begin); await c.handle('voice-insert', insert); await c.handle('voice-submit', submit); await c.handle('voice-submit', submit)
  assert.equal(input.inserts, 1); assert.equal(input.submits, 1); assert.equal(receipts.every(p => p.ok), true)
})
test('two sessions/windows cannot consume the same targeted utterance', async () => {
  const a = consumer(), b = consumer(fakeInput(), 'b', 'other')
  const p = a.event(); await b.c.handle('voice-begin', p); await a.c.handle('voice-begin', p)
  assert.equal(a.receipts.length, 1); assert.equal(b.receipts.length, 0)
})
test('ASR after user edit fails InputActions span guard without fallback', async () => {
  const x = consumer(); await x.c.handle('voice-begin', x.event()); x.input.edit('typed')
  await x.c.handle('voice-insert', x.event({ text: 'voice' })); await x.c.handle('voice-submit', x.event())
  assert.equal(x.input.state.draft, 'typed'); assert.equal(x.input.inserts, 0); assert.equal(x.input.submits, 0); assert.equal(x.receipts.at(-1).ok, false)
})
test('user edits/attachments/references/phase changes after insertion prevent unrelated draft submission', async () => {
  for (const mutate of [i => i.edit('voice plus user text'), i => i.state.attachmentIds.push('file'), i => i.state.occurrences.push({}), i => { i.state.phase = 'sending' }]) {
    const x = consumer(); await x.c.handle('voice-begin', x.event()); await x.c.handle('voice-insert', x.event({ text: 'voice' })); mutate(x.input)
    await x.c.handle('voice-submit', x.event()); assert.equal(x.input.submits, 0)
  }
  const x = consumer(fakeInput('existing user draft')); await x.c.handle('voice-begin', x.event()); await x.c.handle('voice-insert', x.event({ text: 'voice' })); await x.c.handle('voice-submit', x.event()); assert.equal(x.input.submits, 0)
})
test('out-of-order submit, wrong lease/revision and unmounted consumer cannot perform actions', async () => {
  const x = consumer(); await x.c.handle('voice-submit', x.event()); assert.equal(x.input.submits, 0)
  await x.c.handle('voice-begin', x.event()); await x.c.handle('voice-insert', x.event({ lease: 'other', text: 'voice' })); assert.equal(x.input.inserts, 0)
  x.c.dispose(); await x.c.handle('voice-insert', x.event({ text: 'voice' })); assert.equal(x.input.inserts, 0)
})
test('submit exception consumes the operation and never retries or uses a second UI path', async () => {
  const input = fakeInput(); let n = 0; input.actions.submit = () => { n++; throw Error('unknown') }
  const x = consumer(input); await x.c.handle('voice-begin', x.event()); await x.c.handle('voice-insert', x.event({ text: 'voice' }))
  const p = x.event(); await x.c.handle('voice-submit', p); await x.c.handle('voice-submit', p)
  assert.equal(n, 1); assert.equal(x.receipts.at(-1).ok, false)
})

test('HTTP voice begin/insertion ACK and submit are bound to one mounted owner/session/epoch/lease', async t => {
  const { server: s, url } = await startTestServer(t, { allowedOrigins: [origin], listSessions: () => [{ id: 'a' }, { id: 'b' }] })
  const post = async (path, body, headers) => {
    const r = await fetch(url + path, { method: 'POST', headers: { 'Content-Type': 'application/json', ...headers }, body: JSON.stringify(body) })
    return { status: r.status, body: await r.json() }
  }
  const ticket = await post('/pair/ticket', { sessionId: 'a', origin }, { Authorization: `Bearer ${s.tokens.voice}` })
  const paired = await post('/pair', { sessionId: 'a', code: ticket.body.code }, { Origin: origin })
  const webHeaders = { Origin: origin, Authorization: `Bearer ${paired.body.token}` }, nativeHeaders = { Authorization: `Bearer ${s.tokens.voice}` }
  const stream = await openSse(url + '/events', { headers: webHeaders }); t.after(() => stream.close())
  await waitFor(() => s.voiceOwners.has('a'))
  assert.equal((await post('/voice/begin', { sessionId: 'b', utteranceId: 'wrong' }, nativeHeaders)).status, 409)
  const input = fakeInput(), c = createVoiceConsumer({ ...paired.body, inputActions: input.actions, readInput: () => input.state, ack: p => post('/voice/ack', p, webHeaders) })
  const beginPending = post('/voice/begin', { sessionId: 'a', utteranceId: 'u' }, nativeHeaders)
  const begin = await stream.next(); await c.handle(begin.event, begin.data); const beginResult = await beginPending
  assert.equal(beginResult.status, 200); assert.equal(beginResult.body.status, 'captured')
  const binding = { sessionId: 'a', utteranceId: 'u', epoch: beginResult.body.epoch, lease: beginResult.body.lease, revision: 1 }
  assert.equal((await post('/voice/submit', binding, nativeHeaders)).status, 409)
  const inserted = post('/voice/draft', { ...binding, text: 'voice' }, nativeHeaders)
  const insert = await stream.next(); await c.handle(insert.event, insert.data); assert.equal((await inserted).body.status, 'inserted')
  const submitted = post('/voice/submit', binding, nativeHeaders)
  const submit = await stream.next(); await c.handle(submit.event, submit.data); assert.equal((await submitted).body.status, 'submit-requested')
  assert.equal((await post('/voice/submit', binding, nativeHeaders)).status, 409)
  assert.equal(input.submits, 1); stream.close(); await waitFor(() => !s.voiceOwners.has('a'))
  assert.equal((await post('/voice/submit', binding, nativeHeaders)).status, 409)
})

// Exercise actual browser registration: desktop input now uses the OS composer.
test('status-only browser module has no pairing slot, network or input side effects', () => {
  const vm = require('node:vm'), fs = require('node:fs'), path = require('node:path')
  let plugin, registrations = 0
  const forbidden = () => { throw Error('status module must stay inert') }
  const context = { fetch: forbidden, WebSocket: forbidden, EventSource: forbidden,
    window: { __ModuleLoader__: { load(spec) { registrations++; assert.equal(spec.id, '@hachimodock/dsh-bridge'); plugin = spec.factory(forbidden) } } } }
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../client.js'), 'utf8'), context)
  assert.equal(registrations, 1)
  assert.deepEqual([...plugin.inject], [])
  plugin.apply({ slots: { inject: forbidden, register: forbidden }, inputActions: { insertText: forbidden, submit: forbidden } })
})
