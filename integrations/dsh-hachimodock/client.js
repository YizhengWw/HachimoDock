/* Session-scoped InputActions only. No approval UI, DOM, keyboard or retry fallback. */
;(function () {
'use strict'

function opaqueId(value, maximum = 512) {
  return typeof value === 'string' && value.length > 0 && value.length <= maximum
}

function supportedInputSnapshot(state) {
  return state !== null && typeof state === 'object'
    && ['plain', 'claimed', 'adjudicating', 'submitting'].includes(state.phase)
    && typeof state.draft === 'string'
    && Number.isSafeInteger(state.draftRev) && state.draftRev >= 0
    && Array.isArray(state.attachmentIds) && Array.isArray(state.occurrences)
}

function supportedInsertionSpan(span) {
  return span !== null && typeof span === 'object' && !Array.isArray(span)
    && Number.isSafeInteger(span.start) && span.start >= 0
    && Number.isSafeInteger(span.end) && span.end >= span.start
    && Number.isSafeInteger(span.draftRev) && span.draftRev >= 0
}

function supportedInputActions(actions) {
  return actions !== null && typeof actions === 'object'
    && ['captureInsertion', 'insertText', 'submit'].every(k => typeof actions[k] === 'function')
}

function createVoiceConsumer({ sessionId, clientId, epoch, inputActions, readInput, ack, isCurrent = () => true, onUnavailable = () => {} }) {
  let disposed = false
  const utterances = new Map(), receipts = new Map()
  async function handle(type, p) {
    if (disposed || !isCurrent() || !p || p.sessionId !== sessionId || p.owner !== clientId || p.epoch !== epoch
      || !opaqueId(p.actionId) || !opaqueId(p.utteranceId) || !opaqueId(p.lease)) return
    const prior = receipts.get(p.actionId)
    if (prior) { await ack(prior); return }
    let ok = false, unavailable = null
    try {
      const state = readInput(), u = utterances.get(p.utteranceId)
      if (!supportedInputActions(inputActions) || !supportedInputSnapshot(state)) {
        unavailable = 'unsupported'
      } else if (type === 'voice-begin' && !u && utterances.size < 32 && state.phase === 'plain') {
        const span = inputActions.captureInsertion()
        if (!supportedInsertionSpan(span) || span.draftRev !== state.draftRev) unavailable = 'unsupported'
        else if (isCurrent()) {
          utterances.set(p.utteranceId, { span: { start: span.start, end: span.end, draftRev: span.draftRev }, lease: p.lease, state: 'captured' })
          ok = true
        }
      } else if (type === 'voice-insert' && u?.state === 'captured' && u.lease === p.lease
        && typeof p.text === 'string' && p.text.trim() && p.text.length <= 4000
        && Number.isSafeInteger(p.revision) && p.revision >= 0) {
        u.state = 'insertion-unknown' // consume before user code; never replay an uncertain side effect
        if (isCurrent()) {
          const inserted = inputActions.insertText(p.text, u.span)
          if (typeof inserted !== 'boolean') unavailable = 'unsupported'
          else if (inserted) {
            const span = inputActions.captureInsertion()
            if (!supportedInsertionSpan(span)) unavailable = 'unsupported'
            else {
              u.state = 'inserted'; u.text = p.text; u.revision = p.revision; u.draftRev = span.draftRev
              ok = true
            }
          }
        }
      } else if (type === 'voice-submit' && u?.state === 'inserted' && u.lease === p.lease
        && u.revision === p.revision && state.phase === 'plain' && state.draft === u.text
        && state.draftRev === u.draftRev && state.attachmentIds.length === 0 && state.occurrences.length === 0) {
        const span = inputActions.captureInsertion()
        if (!supportedInsertionSpan(span)) unavailable = 'unsupported'
        else if (span.draftRev === u.draftRev && isCurrent()) {
          u.state = 'submit-unknown' // submit():void means requested, never delivered
          const submitted = inputActions.submit()
          if (submitted !== undefined) unavailable = 'unsupported'
          else ok = true
        }
      }
    } catch { unavailable = 'unconfirmed'; ok = false }
    const receipt = { actionId: p.actionId, utteranceId: p.utteranceId, sessionId, epoch, lease: p.lease, ok }
    receipts.set(p.actionId, receipt)
    if (receipts.size > 128) { disposed = true; throw new Error('voice lease exhausted') }
    if (unavailable) disposed = true
    try { await ack(receipt) } finally { if (unavailable) onUnavailable(unavailable) }
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
  if (typeof loader?.load !== "function") return false
  // Desktop input is owned by the same OS composer adapter as ChatGPT and
  // WorkBuddy. Keep the status resource registered, without a pairing dock,
  // editor injection, automatic form changes or browser credentials.
  loader.load({ id: "@hachimodock/dsh-bridge", factory() {
    return { inject: [], apply() {} }
  } })
  return true
}
if (typeof module !== 'undefined' && module.exports) module.exports = { createVoiceConsumer, consumeEvents }
if (typeof window !== 'undefined' && window.__ModuleLoader__) install(window.__ModuleLoader__)

})();
