'use strict'

// Hermes → HachimoDock state mapping (issue #28, M1).
// Pure functions only: the monitor owns the database, the tests own the truth.
//
// The deskpet vocabulary is exactly these 11 states; the bridge's
// mapClawdStateToStatus folds them into pet motion states downstream.
const STATES = Object.freeze([
  'idle', 'thinking', 'working', 'speaking', 'attention', 'error',
  'juggling', 'sweeping', 'carrying', 'notification', 'sleeping',
]);

// A tool result row fails when its JSON content carries an error payload
// (terminal exit codes, is_error flags, or an explicit error object).
// Non-JSON content is a success: Hermes tool results are JSON-shaped.
function isToolFailure(content) {
  if (typeof content !== 'string' || !content) return false;
  let value;
  try { value = JSON.parse(content); } catch { return false; }
  if (!value || typeof value !== 'object') return false;
  if (value.is_error === true || value.error) return true;
  return Number.isFinite(value.exit_code) && value.exit_code !== 0;
}

function hasText(content) {
  return typeof content === 'string' && content.trim().length > 0;
}

/**
 * Map one Hermes message row to a state event, or null when the row carries
 * no pet-visible signal (session_meta, interrupted/empty assistant output).
 * @param {{role:string, content:string, finish_reason?:string, tool_name?:string}} row
 */
function messageEvent(row) {
  if (!row || typeof row !== 'object') return null;
  const role = typeof row.role === 'string' ? row.role : '';
  const finish = typeof row.finish_reason === 'string' ? row.finish_reason : '';

  if (role === 'user') return { state: 'working', event: 'user_message' };
  if (role === 'assistant') {
    if (finish === 'tool_calls') return { state: 'thinking', event: 'tool_call_dispatch' };
    if (finish === 'stop' || finish === 'length') return { state: 'attention', event: 'turn_end' };
    // finish_reason unset while the turn is still streaming; an interrupted
    // turn carries no content, so it must not read as speaking.
    if (!finish && hasText(row.content)) return { state: 'speaking', event: 'assistant_streaming' };
    return null;
  }
  if (role === 'tool') {
    if (isToolFailure(row.content)) {
      return { state: 'error', event: 'tool_failed', tool: typeof row.tool_name === 'string' ? row.tool_name : '' };
    }
    return { state: 'working', event: 'tool_result', tool: typeof row.tool_name === 'string' ? row.tool_name : '' };
  }
  return null; // session_meta and anything unknown stays silent
}

/**
 * Elevate the row-derived state with cross-table context. Precedence:
 * error stays, then waiting-for-human, then session end, then the
 * multi-agent / maintenance signals, then the row's own state.
 * @param {{state:string, event:string, tool?:string}|null} event
 * @param {{pendingClarify?:boolean, sessionEnded?:boolean, activeDelegation?:boolean,
 *          compressionActive?:boolean, compactedThisPoll?:boolean, activeWorker?:boolean}} ctx
 */
function reduce(event, ctx = {}) {
  const base = event && STATES.includes(event.state) ? event : { state: 'idle', event: 'no_activity' };
  if (base.state === 'error') return base;
  if (ctx.pendingClarify) return { state: 'notification', event: 'clarify_pending' };
  if (ctx.sessionEnded) return { state: 'sleeping', event: 'session_ended' };
  if (ctx.activeDelegation) return { state: 'juggling', event: 'delegation_active' };
  if (ctx.compressionActive || ctx.compactedThisPoll) return { state: 'sweeping', event: 'compression_active' };
  if (ctx.activeWorker) return { state: 'carrying', event: 'worker_active' };
  return base;
}

module.exports = { STATES, messageEvent, reduce, isToolFailure };
