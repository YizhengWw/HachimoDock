'use strict'

// Capability contract, not a version check: live Session notifications with
// seq == session.seq - 1, turn-enclosed calls, and correlated tool results.
// Never fold persisted events or read question/answer text into state caches.
const LIMIT = 64
const idOf = value => typeof value === 'string' && value.length > 0 && value.length <= 256 ? value : null
const ordinal = value => Number.isSafeInteger(value) && value >= 0
const record = value => value !== null && typeof value === 'object' && !Array.isArray(value)
const QUESTION_TOOLS = new Set(['ask_user_question'])

function turnEndState(data) {
  const kind = data?.reason?.kind
  if (kind === 'completed') return 'done'
  if (kind === 'error') return 'error'
  if (['aborted', 'interrupted', 'blocked', 'max-tokens', 'forked', 'cancelled', 'canceled'].includes(kind)) return 'stopped'
  return 'unknown'
}

function resultCallId(data) {
  // rc5 uses message.toolCallId; retain the older structurally compatible seam.
  const fields = [data?.message?.toolCallId, data?.message?.callId, data?.callId]
    .filter(value => value !== undefined)
  if (!fields.length || fields.some(value => idOf(value) === null || value !== fields[0])) return null
  return fields[0]
}

function questionResult(data) {
  if (data.message?.isError === true || record(data.error)) return 'failed'
  const content = data.message?.content
  if (!Array.isArray(content) || content.length !== 1) return 'unknown'
  const block = content[0]
  if (block?.type !== 'text' || typeof block.text !== 'string' || block.text.length > 32768) return 'unknown'
  try {
    const value = JSON.parse(block.text)
    if (record(value) && value.pending === true) return 'continued'
    if (record(value) && Array.isArray(value.answers)) return 'answered'
  } catch { /* Unknown result shapes must not pretend the human answered. */ }
  return 'unknown'
}

function createStateContract({ limit = 256 } = {}) {
  const sessions = new Map()
  let currentEpoch = null
  function bind(session, epoch, beforeSeq) {
    if (!idOf(epoch)) { sessions.clear(); currentEpoch = null; return null }
    if (epoch !== currentEpoch) { sessions.clear(); currentEpoch = epoch }
    if (!idOf(session?.id) || !ordinal(session.seq)) return null
    let entry = sessions.get(session.id)
    if (!entry || entry.session !== session) {
      entry = { session, state: 'unknown', waitKind: null, revision: 0,
        seq: beforeSeq ?? session.seq - 1, turn: null, lastTurn: -1,
        calls: new Map(), approvals: new Map(), continued: new Map(), uncertain: false }
      sessions.delete(session.id)
      sessions.set(session.id, entry)
      if (sessions.size > limit) sessions.delete(sessions.keys().next().value)
    }
    return entry
  }
  function set(entry, state, waitKind = null) {
    if (entry.state !== state || entry.waitKind !== waitKind) entry.revision += 1
    entry.state = state
    entry.waitKind = waitKind
  }
  function invalidate(entry) {
    entry.turn = null
    entry.calls.clear(); entry.approvals.clear(); entry.continued.clear()
    entry.uncertain = true
    set(entry, 'unknown')
  }
  function active(entry) {
    if (entry.uncertain || entry.turn === null) return set(entry, 'unknown')
    if (entry.approvals.size) return set(entry, 'waiting_user', 'toolapproval')
    if ([...entry.calls.values()].some(call => call.question)) return set(entry, 'waiting_user', 'question')
    set(entry, entry.calls.size ? 'tool_running' : 'working')
  }
  function view(entry, epoch) {
    return { state: entry?.state ?? 'unknown', waitKind: entry?.waitKind ?? null,
      revision: entry?.revision ?? 0, epoch: idOf(epoch), sourceSeq: entry?.seq ?? null }
  }
  function snapshot(session, epoch) { return view(bind(session, epoch), epoch) }
  function forget(session) {
    if (sessions.get(session?.id)?.session === session) sessions.delete(session.id)
  }
  function observe(session, event, epoch) {
    // This getter is the installed Session append contract. Missing capabilities
    // are unknown, not an invitation to replay the last event from disk.
    if (!ordinal(event?.seq) || !ordinal(session?.seq)) {
      const entry = bind(session, epoch)
      if (entry) invalidate(entry)
      return { accepted: false, changed: true, ...view(entry, epoch) }
    }
    if (event.seq !== session.seq - 1) return { accepted: false, changed: false }
    const entry = bind(session, epoch, event.seq - 1)
    if (!entry || event.seq <= entry.seq) return { accepted: false, changed: false }
    const previousRevision = entry.revision
    const gap = event.seq !== entry.seq + 1
    entry.seq = event.seq
    if (gap) invalidate(entry)
    const data = record(event.data) ? event.data : {}
    let accepted = true
    let call = null
    const sameTurn = ordinal(data.turn) && data.turn === entry.turn
    switch (event.type) {
      case 'turn/start':
        if (!ordinal(data.turn)) { invalidate(entry); break }
        if (data.turn <= entry.lastTurn) { accepted = false; break }
        entry.turn = data.turn; entry.lastTurn = data.turn
        entry.calls.clear(); entry.approvals.clear(); entry.uncertain = false
        active(entry)
        break
      case 'turn/end': {
        if (!ordinal(data.turn)) { invalidate(entry); break }
        if (!sameTurn) { accepted = false; break }
        const state = turnEndState(data)
        entry.turn = null; entry.calls.clear(); entry.approvals.clear()
        if (state !== 'done') entry.continued.clear()
        set(entry, state === 'done' && entry.continued.size ? 'waiting_user' : state,
          state === 'done' && entry.continued.size ? 'question' : null)
        break
      }
      case 'tool/call': {
        if (!ordinal(data.turn)) { invalidate(entry); break }
        if (!sameTurn) { accepted = false; break }
        const id = idOf(data.callId)
        const name = typeof data.name === 'string' && data.name.length > 0 && data.name.length <= 120 ? data.name : null
        if (!id || !name || entry.calls.size >= LIMIT || entry.calls.has(id) || entry.continued.has(id)) {
          invalidate(entry); break
        }
        call = { callId: id, name, question: QUESTION_TOOLS.has(name), turn: entry.turn }
        entry.calls.set(id, call)
        active(entry)
        break
      }
      case 'tool/result': {
        if (!ordinal(data.turn)) { invalidate(entry); break }
        if (!sameTurn) { accepted = false; break }
        const id = resultCallId(data)
        if (!id) { invalidate(entry); break }
        call = id && entry.calls.get(id)
        if (!call || call.turn !== entry.turn) { accepted = false; break }
        entry.calls.delete(id)
        if (call.question) {
          const result = questionResult(data)
          if (result === 'continued') {
            if (entry.continued.size >= LIMIT) { invalidate(entry); break }
            entry.continued.set(id, call)
          } else if (result === 'unknown') { invalidate(entry); break }
        }
        active(entry)
        break
      }
      case 'user/message': {
        // Only this committed live Session event proves admission. A submit or
        // queued inbox notification never enters this branch. Some contracts may
        // admit before the next turn/start; settle the exact continued call then,
        // but active() keeps the lifecycle unknown until that turn is observed.
        if (data.source?.kind !== 'user-question-reply') break
        const id = idOf(data.source.callId)
        if (!id || !entry.continued.has(id)) { accepted = false; break }
        if (data.turn !== undefined
            && (entry.turn !== null ? !sameTurn : !ordinal(data.turn) || data.turn <= entry.lastTurn)) {
          accepted = false; break
        }
        entry.continued.delete(id)
        active(entry)
        break
      }
      case 'approval/asked': {
        if (data.turn !== undefined && !sameTurn) { accepted = false; break }
        const id = idOf(data.id)
        if (entry.turn === null) { accepted = false; break }
        if (!id || entry.approvals.size >= LIMIT || entry.approvals.has(id)) { invalidate(entry); break }
        // The installed audit can omit turn; then an exact current call is
        // required. A new uncorrelated approval id cannot prove currentness.
        if (data.turn === undefined && data.callId === undefined) { invalidate(entry); break }
        if (data.callId !== undefined) {
          if (!idOf(data.callId)) { invalidate(entry); break }
          if (!entry.calls.has(data.callId)) { accepted = false; break }
        }
        entry.approvals.set(id, entry.turn)
        active(entry)
        break
      }
      case 'approval/decided': {
        if (data.turn !== undefined && !sameTurn) { accepted = false; break }
        const id = idOf(data.id)
        if (!id) { invalidate(entry); break }
        if (!id || entry.turn === null || entry.approvals.get(id) !== entry.turn) { accepted = false; break }
        entry.approvals.delete(id)
        if (!['allowed-once', 'rejected', 'cancelled', 'unavailable'].includes(data.outcome)) { invalidate(entry); break }
        active(entry)
        break
      }
      case 'step/start':
        if (!ordinal(data.turn)) { invalidate(entry); break }
        if (sameTurn) active(entry)
        else accepted = false
        break
      default:
        if (typeof event.type === 'string' && /^(turn|task|tool|user-questions|approval)\//.test(event.type)
            && !['tool/ptc-dispatch', 'approval/policy'].includes(event.type)) invalidate(entry)
        // PTC can contain timed human waits; an unsupported dispatch cannot be
        // presented as confidently working even if another tool is executing.
        else if (event.type === 'tool/ptc-dispatch') invalidate(entry)
        break
    }
    return { accepted, changed: previousRevision !== entry.revision, call, ...view(entry, epoch) }
  }
  return { observe, snapshot, forget }
}

module.exports = { createStateContract, turnEndState, resultCallId }
