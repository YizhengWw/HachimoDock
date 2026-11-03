/* Session-scoped InputActions only. No approval UI, DOM, keyboard or retry fallback. */
'use strict'

function createVoiceConsumer({ sessionId, clientId, epoch, inputActions, readInput, ack }) {
  let disposed = false
  const utterances = new Map(), receipts = new Map()
  async function handle(type, p) {
    if (disposed || !p || p.sessionId !== sessionId || p.owner !== clientId || p.epoch !== epoch || typeof p.actionId !== 'string' || typeof p.utteranceId !== 'string' || typeof p.lease !== 'string') return
    const prior = receipts.get(p.actionId)
    if (prior) { await ack(prior); return }
    let ok = false
    try {
      const state = readInput()
      const u = utterances.get(p.utteranceId)
      if (type === 'voice-begin' && !u && utterances.size < 32 && state.phase === 'plain') {
        const span = inputActions.captureInsertion()
        utterances.set(p.utteranceId, { span, lease: p.lease, state: 'captured' })
        ok = true
      } else if (type === 'voice-insert' && u?.state === 'captured' && u.lease === p.lease && typeof p.text === 'string' && p.text.trim() && p.text.length <= 4000 && Number.isSafeInteger(p.revision)) {
        u.state = 'insertion-unknown' // consume before calling user code
        ok = inputActions.insertText(p.text, u.span) === true
        if (ok) {
          u.state = 'inserted'; u.text = p.text; u.revision = p.revision
          u.draftRev = inputActions.captureInsertion().draftRev
        }
      } else if (type === 'voice-submit' && u?.state === 'inserted' && u.lease === p.lease && u.revision === p.revision && state.phase === 'plain' && state.draft === u.text && state.draftRev === u.draftRev && !state.attachmentIds?.length && !state.occurrences?.length && inputActions.captureInsertion().draftRev === u.draftRev) {
        u.state = 'submit-unknown' // void submit is a request, never a delivery ACK
        inputActions.submit()
        ok = true
      }
    } catch { ok = false }
    const receipt = { actionId: p.actionId, utteranceId: p.utteranceId, sessionId, epoch, lease: p.lease, ok }
    receipts.set(p.actionId, receipt)
    // Retain terminal IDs for this bounded lease. At capacity the stream closes;
    // eviction could turn an old action into a new side effect.
    if (receipts.size > 128) { disposed = true; throw new Error('voice lease exhausted') }
    await ack(receipt)
  }
  return { handle, dispose() { disposed = true; utterances.clear(); receipts.clear() } }
}

async function consumeEvents(response, signal, handle) {
  if (!response.ok || !response.body) throw new Error('voice stream unavailable')
  const reader = response.body.getReader(), decoder = new TextDecoder('utf-8', { fatal: true })
  let buffer = ''
  try {
    while (!signal.aborted) {
      const { done, value } = await reader.read()
      if (done) throw new Error('voice stream disconnected')
      buffer += decoder.decode(value, { stream: true }).replace(/\r\n/g, '\n')
      if (buffer.length > 65536) throw new Error('voice frame too large')
      let at
      while ((at = buffer.indexOf('\n\n')) >= 0) {
        const block = buffer.slice(0, at); buffer = buffer.slice(at + 2)
        let type = '', data = ''
        for (const line of block.split('\n')) {
          if (line.startsWith('event:')) type = line.slice(6).trim()
          if (line.startsWith('data:')) data += line.slice(5).trimStart()
        }
        if (['voice-begin', 'voice-insert', 'voice-submit'].includes(type) && data) await handle(type, JSON.parse(data))
      }
    }
  } finally { await reader.cancel().catch(() => {}) }
}

function install(loader) {
  loader.load({ id: '@hachimodock/dsh-bridge', factory(require) {
    const React = require('react')
    function VoiceDock(props) {
      if (typeof props.sessionId !== 'string' || typeof props.useInput !== 'function' || !['captureInsertion', 'insertText', 'submit'].every(k => typeof props.inputActions?.[k] === 'function')) {
        return React.createElement('span', null, 'HachimoDock voice unavailable: unsupported scoped InputActions')
      }
      return React.createElement(ScopedVoiceDock, props)
    }
    function ScopedVoiceDock(props) {
      const state = props.useInput(s => s), input = React.useRef(state); input.current = state
      const [code, setCode] = React.useState(''), [pair, setPair] = React.useState(null)
      const [status, setStatus] = React.useState('未配对：从本地受保护的 capability descriptor 获取一次性配对码')
      const base = 'http://127.0.0.1:17892'
      React.useEffect(() => {
        if (!pair || pair.sessionId !== props.sessionId) return
        const controller = new AbortController()
        const post = async (path, body) => {
          const r = await fetch(base + path, { method: 'POST', headers: { Authorization: `Bearer ${pair.token}`, 'Content-Type': 'application/json' }, body: JSON.stringify(body), signal: controller.signal, redirect: 'error', cache: 'no-store' })
          if (!r.ok) throw new Error('voice receipt unavailable')
          return r.json()
        }
        const consumer = createVoiceConsumer({ ...pair, inputActions: props.inputActions, readInput: () => input.current, ack: b => post('/voice/ack', b) })
        fetch(base + '/events', { headers: { Authorization: `Bearer ${pair.token}` }, signal: controller.signal, redirect: 'error', cache: 'no-store' })
          .then(r => consumeEvents(r, controller.signal, consumer.handle))
          .catch(() => { consumer.dispose(); controller.abort(); setStatus('语音连接已关闭；未确认的动作不会重发') })
        return () => { consumer.dispose(); controller.abort() }
      }, [pair, props.sessionId, props.inputActions])
      const pairNow = async () => {
        const oneTimeCode = code; setCode('')
        try {
          const r = await fetch(base + '/pair', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ code: oneTimeCode, sessionId: props.sessionId }), redirect: 'error', cache: 'no-store' })
          const p = await r.json()
          if (!r.ok || p.sessionId !== props.sessionId || typeof p.token !== 'string') throw new Error('unpaired')
          setPair(p); setStatus('此会话已配对；切换/卸载后重新配对，不自动重发')
        } catch { setStatus('配对不可用：检查已配置的精确 Origin 与一次性配对码') }
      }
      return React.createElement('div', null,
        React.createElement('span', { role: 'status' }, status),
        !pair && React.createElement('input', { type: 'password', autoComplete: 'off', value: code, onChange: e => setCode(e.target.value), 'aria-label': 'HachimoDock pairing code' }),
        !pair && React.createElement('button', { type: 'button', onClick: pairNow, disabled: !code }, '配对此会话'))
    }
    return { inject: ['slots'], apply(ctx) {
      ctx.slots.inject('conversation.composer.dock', () => ctx.slots.register({ name: 'conversation.composer.dock', id: 'hachimodock-voice-dock', order: 999 }, VoiceDock))
    } }
  } })
}
if (typeof module !== 'undefined' && module.exports) module.exports = { createVoiceConsumer, consumeEvents }
if (typeof window !== 'undefined' && window.__ModuleLoader__) install(window.__ModuleLoader__)
