'use strict'

/**
 * DeepSeek Harness (Cordis) boundary for the HachimoDock bridge.
 *
 * This module is the only place that touches a DSH `ctx`. It translates
 * Cordis surfaces into the plain adapter deps `server.js` consumes:
 *
 *   - `ctx.sessions.list()/get(id)`          -> `GET /sessions`, `POST /inject`
 *   - `ctx.agents.get(id)`, `agent.followup()`  -> prompt delivery
 *   - `session/event`, `agent/assistant-stream` -> `/inject` SSE token/tool/done
 *   - `ctx.on('approval/request', (request, next) => outcome)` -> `/approval/decision`
 *
 * Cordis facts this file relies on (verified against the installed DSH
 * `@deepseek-ai/cordis` 4.0.5, `dsh-user-approval` 0.2.1-rc.3 and
 * `dsh-agent` 0.2.1-rc.3 builds):
 *   - Waterfall listeners receive the dispatch arguments plus a final `next`
 *     callback; returning an approval outcome (`allowed-once` | `rejected` |
 *     `cancelled` | `unavailable`) answers the request, calling `next()`
 *     delegates to the remaining answerers.
 *   - `session/event` listeners receive `(session, event)`; `session/created`
 *     and `session/disposed` receive `(session)`.
 *   - Agent-scoped emit events spread their payload and add `agent`; a global
 *     `ctx.on('agent/assistant-stream', ...)` listener therefore sees
 *     `{frame, agent}`.
 *   - `ctx.sessions` / `ctx.agents` are the documented services.
 *
 * Nothing here is imported from DSH: the plugin is plain CommonJS with Node
 * built-ins only, and every DSH object is used structurally. `createBridge()`
 * accepts a fake `ctx` in tests.
 *
 * @module @hachimodock/dsh-bridge/dsh-bridge
 */

const { randomUUID } = require('node:crypto')
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { BridgeServer, DEFAULT_PORT, DEFAULT_APPROVAL_TIMEOUT_MS, boundString } = require('./server')
const DEFAULT_BRIDGE_PROFILE = path.join(os.homedir(), '.claw-pet', 'pet-bridge.json')
const NEVER_APPROVAL_TIMEOUT_MS = 99999

/** Agent identity reported to HachimoDock; fixed by the v0 contract. */
const AGENT_ID = 'deepseek-harness'
/** Plugin display name registered with Cordis. */
const PLUGIN_NAME = 'hachimodock-dsh-bridge'
/** Required Cordis services: the plugin waits for them before mounting. */
const INJECT = ['sessions', 'agents']

/** Default prompt text characters mirrored to `/inject` token events. */
const MAX_INJECT_TOKEN_CHARS = 256 * 1024
/** Default bound on one injected prompt before reporting `inject-timeout`. */
const DEFAULT_INJECT_TIMEOUT_MS = 10 * 60 * 1000
/** Bounded caches for ids observed on live session events. */
const CACHE_LIMIT = 256

/**
 * Read a positive integer from an environment variable.
 * @param {string|undefined} raw - raw environment value.
 * @returns {number|undefined} parsed value, or undefined.
 */
function positiveIntFromEnv(raw) {
  if (typeof raw !== 'string' || raw.trim() === '') return undefined
  const value = Number(raw)
  return Number.isSafeInteger(value) && value > 0 ? value : undefined
}

/**
 * Merge defaults, bundle config, and environment variables into options.
 *
 * Precedence: environment variable > bundle config > built-in default.
 * @param {object} [config] - config from the Cordis row.
 * @returns {{port:number, approvalTimeoutMs:number, approvalFallback:'unavailable'|'delegate', injectTimeoutMs:number, maxInjectTokenChars:number}} resolved options.
 */
function normalizeOptions(config = {}) {
  const options = config !== null && typeof config === 'object' ? config : {}
  const port =
    positiveIntFromEnv(process.env.HACHIMODOCK_DSH_PORT) ??
    (Number.isSafeInteger(options.port) && options.port > 0 ? options.port : DEFAULT_PORT)
  const approvalTimeoutMs =
    positiveIntFromEnv(process.env.HACHIMODOCK_DSH_APPROVAL_TIMEOUT_MS) ??
    (Number.isSafeInteger(options.approvalTimeoutMs) && options.approvalTimeoutMs > 0
      ? options.approvalTimeoutMs
      : DEFAULT_APPROVAL_TIMEOUT_MS)
  const fallback =
    process.env.HACHIMODOCK_DSH_APPROVAL_FALLBACK ??
    (typeof options.approvalFallback === 'string' ? options.approvalFallback : 'unavailable')
  const approvalFallback = fallback === 'delegate' ? 'delegate' : 'unavailable'
  const injectTimeoutMs =
    positiveIntFromEnv(process.env.HACHIMODOCK_DSH_INJECT_TIMEOUT_MS) ??
    (Number.isSafeInteger(options.injectTimeoutMs) && options.injectTimeoutMs > 0
      ? options.injectTimeoutMs
      : DEFAULT_INJECT_TIMEOUT_MS)
  const mirrorNative =
    process.env.HACHIMODOCK_DSH_MIRROR_NATIVE !== '0'
  return { port, approvalTimeoutMs, approvalFallback, mirrorNative, injectTimeoutMs, maxInjectTokenChars: MAX_INJECT_TOKEN_CHARS }
}

/**
 * Build a logger that never lets a logging failure break the bridge.
 * @param {object} ctx - Cordis context (or a test double).
 * @returns {{info: (detail: unknown) => void, warn: (detail: unknown) => void, error: (detail: unknown) => void}} logger.
 */
function safeLogger(ctx) {
  /** @param {'info'|'warn'|'error'} level @param {unknown} detail */
  const log = (level, detail) => {
    try {
      const logger = ctx.logger
      if (!logger) return
      if (typeof logger[level] === 'function') logger[level](detail)
    } catch {
      /* ignore logging failures */
    }
  }
  return {
    info: (detail) => log('info', detail),
    warn: (detail) => log('warn', detail),
    error: (detail) => log('error', detail),
  }
}

/**
 * Remember one bounded key/value pair, evicting the oldest entry.
 * @param {Map<string, unknown>} map - cache to touch.
 * @param {string} key - cache key.
 * @param {unknown} value - value to store.
 * @returns {void}
 */
function remember(map, key, value) {
  if (typeof key !== 'string' || key === '') return
  if (map.has(key)) map.delete(key)
  map.set(key, value)
  if (map.size > CACHE_LIMIT) {
    const oldest = map.keys().next()
    if (!oldest.done) map.delete(oldest.value)
  }
}

/**
 * Read a live session by id through the session store.
 * @param {object} ctx - Cordis context.
 * @param {string} sessionId - session id.
 * @returns {object|undefined} the live session, or undefined.
 */
function getLiveSession(ctx, sessionId) {
  try {
    const sessions = ctx.sessions
    if (sessions && typeof sessions.get === 'function') return sessions.get(sessionId)
  } catch {
    /* fall through */
  }
  return undefined
}

/**
 * Read the full live session list.
 * @param {object} ctx - Cordis context.
 * @returns {object[]} live sessions in store order.
 */
function getLiveSessions(ctx) {
  try {
    const sessions = ctx.sessions
    if (sessions && typeof sessions.list === 'function') {
      const list = sessions.list()
      if (Array.isArray(list)) return list.filter((session) => session !== null && typeof session === 'object')
    }
  } catch {
    /* fall through */
  }
  return []
}

/**
 * Resolve the session a prompt should target.
 *
 * An explicit id must be a live session (no cold restore in v0). `auto` (or an
 * absent id) selects the most recently observed live session, falling back to
 * the newest `session.header.createdAt`.
 *
 * @param {object} bridge - bridge state.
 * @param {string} sessionId - requested session id or `auto`.
 * @returns {object|undefined} target session.
 */
function resolveTargetSession(bridge, sessionId) {
  const id = typeof sessionId === 'string' ? sessionId.trim() : ''
  if (id !== '' && id !== 'auto') return getLiveSession(bridge.ctx, id)
  const sessions = getLiveSessions(bridge.ctx)
  let best
  let bestStamp = -1
  for (const session of sessions) {
    const modified = bridge.lastModified.get(session.id)
    const createdAt = Number.isSafeInteger(session.header?.createdAt) ? session.header.createdAt : 0
    const stamp = Number.isFinite(modified) ? modified : createdAt
    if (stamp >= bestStamp) {
      best = session
      bestStamp = stamp
    }
  }
  return best
}

/**
 * Read the current title of a session from the optional DSH projections.
 * @param {object} bridge - bridge state.
 * @param {object} session - live session.
 * @returns {string|null} bounded title or null.
 */
function titleOf(bridge, session) {
  const tracked = bridge.titleMap.get(session.id)
  if (typeof tracked === 'string') return boundString(tracked, 160) ?? null
  try {
    const sessionTitle = bridge.ctx.get?.('sessionTitle')
    if (sessionTitle && typeof sessionTitle.get === 'function') {
      const snapshot = sessionTitle.get(session)
      if (snapshot && typeof snapshot.title === 'string') return boundString(snapshot.title, 160) ?? null
    }
  } catch {
    /* projection unavailable or session not live */
  }
  try {
    const projections = bridge.ctx.get?.('sessionProjections')
    if (projections && typeof projections.stateOf === 'function') {
      const state = projections.stateOf(session, 'title')
      if (typeof state === 'string') return boundString(state, 160) ?? null
    }
  } catch {
    /* projection service absent */
  }
  return null
}

/**
 * Build the compact session summary served by `GET /sessions` and `/events`.
 * @param {object} bridge - bridge state.
 * @param {object} session - live session.
 * @returns {{id:string,title:string|null,cwd:string|null,lastModified:number,status:'idle'|'running',agentId:string}} summary.
 */
function summarizeSession(bridge, session) {
  let agent
  try {
    agent = bridge.ctx.agents?.get?.(session.id)
  } catch {
    agent = undefined
  }
  const lastModified = bridge.lastModified.get(session.id)
  const createdAt = Number.isSafeInteger(session.header?.createdAt) ? session.header.createdAt : 0
  return {
    id: String(session.id),
    title: titleOf(bridge, session),
    cwd: typeof session.header?.cwd === 'string' ? session.header.cwd : null,
    lastModified: Number.isFinite(lastModified) ? lastModified : createdAt,
    status: agent?.status === 'running' ? 'running' : 'idle',
    state: bridge.sessionStates.get(String(session.id))?.state
      || (agent?.status === 'running' ? 'working' : 'idle'),
    transitionRevision: bridge.sessionStates.get(String(session.id))?.revision || 0,
    agentId: AGENT_ID,
  }
}

function rememberSessionState(bridge, sessionId, state) {
  const previous = bridge.sessionStates.get(sessionId) || { state: 'idle', revision: 0 }
  if (previous.state === state) return
  bridge.sessionStates.set(sessionId, { state, revision: previous.revision + 1 })
  if (bridge.sessionStates.size > CACHE_LIMIT) {
    const oldest = bridge.sessionStates.keys().next()
    if (!oldest.done) bridge.sessionStates.delete(oldest.value)
  }
}

/**
 * Publish one `session` event carrying a fresh compact summary.
 * @param {object} bridge - bridge state.
 * @param {object} session - live session.
 * @returns {void}
 */
function publishSessionSummary(bridge, session) {
  if (!session || typeof session.id !== 'string') return
  try {
    bridge.server.publish('session', { session: summarizeSession(bridge, session) })
  } catch (error) {
    bridge.logger.warn(error)
  }
}

/**
 * Forward one live DSH session event to `/events` as a compact bridge event.
 *
 * Only bounded control-plane data is forwarded: turn boundaries, tool
 * call/result pairs, title updates, and approval audit edges. Transcript
 * content is never mirrored here; `/inject` carries the token stream.
 *
 * @param {object} bridge - bridge state.
 * @param {object} session - session that produced the event.
 * @param {object} event - committed DSH session event.
 * @returns {void}
 */
function forwardSessionEvent(bridge, session, event) {
  if (!session || typeof session.id !== 'string' || !event || typeof event.type !== 'string') return
  const sessionId = session.id
  const time = Number.isSafeInteger(event.time) ? event.time : bridge.server.now()
  bridge.lastModified.set(sessionId, time)
  if (bridge.lastModified.size > CACHE_LIMIT) {
    const oldest = bridge.lastModified.keys().next()
    if (!oldest.done) bridge.lastModified.delete(oldest.value)
  }
  const data = event.data !== null && typeof event.data === 'object' ? event.data : {}
  switch (event.type) {
    case 'session/title': {
      const title = boundString(data.title, 160) ?? ''
      remember(bridge.titleMap, sessionId, title)
      bridge.server.publish('session-title', { sessionId, title })
      return
    }
    case 'turn/start':
      rememberSessionState(bridge, sessionId, 'working')
      bridge.server.publish('turn', {
        sessionId,
        phase: 'start',
        turn: Number.isSafeInteger(data.turn) ? data.turn : undefined,
      })
      return
    case 'turn/end':
      rememberSessionState(bridge, sessionId, 'done')
      bridge.server.publish('turn', {
        sessionId,
        phase: 'end',
        turn: Number.isSafeInteger(data.turn) ? data.turn : undefined,
      })
      return
    case 'tool/call': {
      const callId = typeof data.callId === 'string' ? data.callId : ''
      const name = boundString(data.name, 120) ?? 'tool'
      remember(bridge.toolNames, `${sessionId}:${callId}`, name)
      // Tool-running is an active device state, not just a transient event.
      rememberSessionState(bridge, sessionId, 'tool_running')
      bridge.server.publish('tool', { sessionId, phase: 'start', name, callId: callId || undefined, ok: true })
      return
    }
    case 'tool/result': {
      const callId =
        typeof data.message?.callId === 'string'
          ? data.message.callId
          : typeof data.callId === 'string'
            ? data.callId
            : ''
      const key = `${sessionId}:${callId}`
      const name = bridge.toolNames.get(key) ?? 'tool'
      bridge.toolNames.delete(key)
      rememberSessionState(bridge, sessionId, 'working')
      bridge.server.publish('tool', {
        sessionId,
        phase: 'end',
        name,
        callId: callId || undefined,
        ok: data.message?.isError !== true,
      })
      return
    }
    case 'approval/asked': {
      const approvalId = typeof data.id === 'string' ? data.id : ''
      remember(bridge.askedApprovals, sessionId, {
        approvalId,
        toolName: typeof data.toolName === 'string' ? data.toolName : '',
        at: bridge.server.now(),
      })
      bridge.server.publish('approval-asked', {
        sessionId,
        approvalId,
        tool: boundString(data.toolName, 120) ?? 'unknown',
      })
      return
    }
    case 'approval/decided': {
      const approvalId = typeof data.id === 'string' ? data.id : ''
      const requestId = bridge.approvalLinks.get(approvalId)
      bridge.approvalLinks.delete(approvalId)
      bridge.server.publish('approval-resolved', {
        sessionId,
        approvalId,
        ...(requestId === undefined ? {} : { requestId }),
        outcome: typeof data.outcome === 'string' ? data.outcome : 'unavailable',
      })
      return
    }
    default:
      return
  }
}

/**
 * Deliver one prompt into a live DSH agent and emit `/inject` SSE events.
 *
 * The adapter follows the contract's token/tool/done/error vocabulary: live
 * assistant text deltas become `token`, tool call/result pairs become `tool`,
 * and `agent.whenIdle()` settlement becomes `done` with the observed
 * `usage.outputTokens` total (falling back to mirrored text characters).
 *
 * @param {object} bridge - bridge state.
 * @param {{sessionId: string, text: string, emit: (event: object) => void, signal?: AbortSignal}} args - delivery request.
 * @returns {Promise<void>} resolves after the prompt's agent work settles.
 */
async function injectToSession(bridge, args) {
  const emit = typeof args.emit === 'function' ? args.emit : () => {}
  const target = resolveTargetSession(bridge, args.sessionId)
  if (target === undefined) {
    emit({ type: 'error', code: 'session-not-found', message: `no live DSH session matches ${JSON.stringify(args.sessionId)}` })
    return
  }
  let agent
  try {
    agent = bridge.ctx.agents?.get?.(target.id)
  } catch (error) {
    bridge.logger.warn(error)
  }
  if (!agent) {
    emit({ type: 'error', code: 'agent-not-live', message: `session ${target.id} has no live agent` })
    return
  }

  const disposers = []
  const toolNames = new Map()
  let stopped = false
  let tokens = 0
  let mirroredChars = 0
  const cleanup = () => {
    for (const dispose of disposers.splice(0)) {
      try {
        if (typeof dispose === 'function') dispose()
      } catch {
        /* ignore */
      }
    }
  }
  const forward = (event) => {
    if (stopped) return
    try {
      emit(event)
    } catch {
      /* a closed SSE consumer must not break delivery */
    }
  }

  // Live token deltas for this agent (agent-scoped emit: payload carries `agent`).
  disposers.push(
    bridge.ctx.on('agent/assistant-stream', (payload) => {
      if (stopped || !payload || payload.agent !== agent) return
      const frame = payload.frame
      if (!frame || frame.type !== 'chunk') return
      const chunk = frame.chunk
      if (!chunk || chunk.type !== 'text-delta' || typeof chunk.text !== 'string' || chunk.text === '') return
      if (mirroredChars >= bridge.options.maxInjectTokenChars) return
      mirroredChars += chunk.text.length
      forward({ type: 'token', text: chunk.text })
    }),
  )
  // Tool activity plus output-token accounting from the durable session log.
  disposers.push(
    bridge.ctx.on('session/event', (session, event) => {
      if (stopped || session !== target || !event) return
      if (event.type === 'assistant/message') {
        const usage = event.data?.usage
        if (Number.isSafeInteger(usage?.outputTokens) && usage.outputTokens >= 0) tokens += usage.outputTokens
        return
      }
      if (event.type === 'tool/call') {
        const callId = typeof event.data?.callId === 'string' ? event.data.callId : ''
        const name = boundString(event.data?.name, 120) ?? 'tool'
        if (callId !== '') remember(toolNames, callId, name)
        forward({ type: 'tool', name, phase: 'start', ok: true })
        return
      }
      if (event.type === 'tool/result') {
        const callId =
          typeof event.data?.message?.callId === 'string'
            ? event.data.message.callId
            : typeof event.data?.callId === 'string'
              ? event.data.callId
              : ''
        const name = toolNames.get(callId) ?? 'tool'
        if (callId !== '') toolNames.delete(callId)
        forward({ type: 'tool', name, phase: 'end', ok: event.data?.message?.isError !== true })
      }
    }),
  )

  try {
    const message = {
      id: randomUUID(),
      role: 'user',
      content: [{ type: 'text', text: args.text }],
      source: { kind: 'user' },
    }
    if (typeof agent.followup !== 'function') {
      forward({ type: 'error', code: 'agent-unsupported', message: 'agent does not expose followup()' })
      return
    }
    // Capture the pre-followup state: real and fake agents switch to running
    // synchronously inside followup(). A turn already in progress can take
    // minutes; device PTT must be accepted into the queue immediately rather
    // than holding the bridge HTTP request until that turn ends.
    const wasRunning = agent.status === 'running' || agent.running === true
    // Always use followup so every device voice turn becomes a visible DSH
    // user message. steer() does not emit a user/message event, which made all
    // voice inputs after the first one invisible in the transcript.
    agent.followup(message)
    if (wasRunning) {
      forward({ type: 'done', sessionId: target.id, tokens: 0, queued: true })
      return
    }
    await waitForAgentIdle(bridge, agent, args.signal)
    forward({ type: 'done', sessionId: target.id, tokens: tokens > 0 ? tokens : mirroredChars })
  } catch (error) {
    forward({ type: 'error', code: 'inject-failed', message: error instanceof Error ? error.message : String(error) })
  } finally {
    stopped = true
    cleanup()
  }
}

/**
 * Wait until an agent is quiescent, using `whenIdle()` when the driver exposes
 * it and falling back to status polling for structural doubles.
 * @param {object} bridge - bridge state.
 * @param {object} agent - live agent.
 * @param {AbortSignal|undefined} signal - caller cancellation.
 * @returns {Promise<void>} resolves at quiescence.
 */
async function waitForAgentIdle(bridge, agent, signal) {
  const deadline = bridge.server.now() + bridge.options.injectTimeoutMs
  const timeoutError = () => new Error(`inject wait exceeded ${bridge.options.injectTimeoutMs}ms`)
  const abortError = () => {
    const error = new Error('inject cancelled')
    error.name = 'AbortError'
    return error
  }
  if (signal?.aborted) throw abortError()
  if (typeof agent.whenIdle === 'function') {
    let removeAbort = null
    const abort = new Promise((_, reject) => {
      if (!signal) return
      const onAbort = () => reject(abortError())
      signal.addEventListener('abort', onAbort, { once: true })
      removeAbort = () => signal.removeEventListener('abort', onAbort)
    })
    try {
      await Promise.race([Promise.resolve(agent.whenIdle()), abort, delayUntil(timeoutError, deadline, bridge.server.now)])
    } finally {
      if (typeof removeAbort === 'function') {
        try {
          removeAbort()
        } catch {
          /* ignore */
        }
      }
    }
    return
  }
  // Structural fallback: poll the documented `status` field.
  while (agent.status === 'running') {
    if (signal?.aborted) throw abortError()
    if (bridge.server.now() >= deadline) throw timeoutError()
    await new Promise((resolve) => {
      const timer = setTimeout(resolve, 100)
      if (typeof timer.unref === 'function') timer.unref()
    })
  }
}

/**
 * Build a rejection that fires at an absolute deadline.
 * @param {() => Error} errorFactory - error factory.
 * @param {number} deadline - absolute epoch milliseconds.
 * @param {() => number} now - clock.
 * @returns {Promise<never>} a promise that rejects at the deadline.
 */
function delayUntil(errorFactory, deadline, now) {
  const waitMs = Math.max(0, deadline - now())
  return new Promise((_, reject) => {
    const timer = setTimeout(() => reject(errorFactory()), waitMs)
    if (typeof timer.unref === 'function') timer.unref()
  })
}

/**
 * Find the DSH `approval/asked` audit edge that belongs to a waterfall request.
 *
 * DSH appends `approval/asked` synchronously before dispatching the answerer
 * waterfall, so the most recent unlinked edge for the same session and tool is
 * the matching one. The link is only used to enrich bridge events; a miss is
 * harmless.
 *
 * @param {object} bridge - bridge state.
 * @param {object} request - approval request from DSH.
 * @returns {string|undefined} the DSH audit approval id, when correlated.
 */
function correlateAskedApproval(bridge, request) {
  const sessionId = request?.agent?.session?.id
  if (typeof sessionId !== 'string') return undefined
  const asked = bridge.askedApprovals.get(sessionId)
  if (!asked) return undefined
  if (asked.toolName !== '' && asked.toolName !== request.toolName) return undefined
  if (bridge.server.now() - asked.at > 5000) return undefined
  bridge.askedApprovals.delete(sessionId)
  return asked.approvalId === '' ? undefined : asked.approvalId
}

/**
 * The Cordis `approval/request` answerer.
 *
 * Broadcasts the request to HachimoDock `/events` subscribers and waits for
 * the device decision:
 *   allow -> `allowed-once`, deny -> `rejected`,
 *   timeout / abort / no connected bridge -> `unavailable` (fail closed).
 *
 * The `unavailable` fallback follows the v0 contract. With
 * `HACHIMODOCK_DSH_APPROVAL_FALLBACK=delegate` (or config
 * `approvalFallback: 'delegate'`) a request with no attached `/events` consumer
 * calls `next()` instead of answering, which keeps a resident DSH approval UI
 * (Web/Desktop or ACP) as the answerer when HachimoDock is not connected.
 *
 * @param {object} bridge - bridge state.
 * @param {{agent?: {session?: {id?: string}}, toolName?: string, reason?: string, displayReason?: string, signal?: AbortSignal}} request - DSH approval request.
 * @param {(() => unknown)|undefined} next - next waterfall answerer.
 * @returns {Promise<'allowed-once'|'rejected'|'unavailable'>} closed outcome.
 */
/**
 * Resolve the live approval timeout from the HachimoDock bridge profile.
 * 0 means wait forever. Absent/invalid falls back to the plugin config.
 * @param {object} bridge - bridge state.
 * @returns {number} timeout in milliseconds; 0 means never.
 */
function resolveApprovalTimeoutMs(bridge) {
  const envValue = process.env.HACHIMODOCK_DSH_APPROVAL_TIMEOUT_MS
  if (typeof envValue === 'string' && envValue.trim() !== '') {
    const parsed = Number(envValue)
    if (Number.isSafeInteger(parsed) && parsed >= 0) return parsed
  }
  try {
    const profilePath = process.env.HACHIMODOCK_BRIDGE_PROFILE || DEFAULT_BRIDGE_PROFILE
    const raw = JSON.parse(fs.readFileSync(profilePath, 'utf8'))
    const value = raw?.dshApprovalTimeoutMs
    if (Number.isSafeInteger(value) && value > 0) return value
  } catch {
    /* profile missing or unreadable: use the plugin default */
  }
  return bridge.options.approvalTimeoutMs
}

/**
 * Cancel the active turn of one live DSH session.
 * @param {object} bridge - bridge state.
 * @param {{sessionId?: string}} args - target session, or auto.
 * @returns {Promise<{ok:boolean, sessionId?:string, interrupted?:boolean, error?:string}>} result.
 */
async function interruptSession(bridge, args = {}) {
  const target = resolveTargetSession(bridge, args.sessionId)
  if (!target) return { ok: false, error: 'session-not-found' }
  let agent
  try {
    agent = bridge.ctx.agents?.get?.(target.id)
  } catch (error) {
    bridge.logger.warn(error)
  }
  if (!agent) return { ok: false, error: 'agent-not-live' }
  const running = agent.status === 'running' || agent.running === true
  if (!running) return { ok: true, sessionId: target.id, interrupted: false, state: 'idle' }
  if (typeof agent.cancel !== 'function') return { ok: false, error: 'agent-unsupported' }
  agent.cancel({ kind: 'user' })
  return { ok: true, sessionId: target.id, interrupted: true, state: 'cancelled' }
}

async function handleApprovalRequest(bridge, request, next) {
  const delegate = typeof next === 'function' ? next : () => Promise.resolve('unavailable')
  try {
    const sessionId = request?.agent?.session?.id
    if (typeof sessionId !== 'string') return 'unavailable'
    if (!bridge.server.hasEventSubscriber()) {
      if (bridge.options.approvalFallback === 'delegate') return await delegate()
      return 'unavailable'
    }
    const approvalId = correlateAskedApproval(bridge, request)
    const prompt = bridge.server.requestApproval({
      agentId: AGENT_ID,
      sessionId,
      tool: typeof request.toolName === 'string' ? request.toolName : 'unknown',
      reason: typeof request.reason === 'string' ? request.reason : undefined,
      displayReason: typeof request.displayReason === 'string' ? request.displayReason : undefined,
      approvalId,
      timeoutMs: resolveApprovalTimeoutMs(bridge),
      signal: request.signal,
    })
    if (approvalId !== undefined) {
      bridge.approvalLinks.set(approvalId, prompt.requestId)
      if (bridge.approvalLinks.size > CACHE_LIMIT) {
        const oldest = bridge.approvalLinks.keys().next()
        if (!oldest.done) bridge.approvalLinks.delete(oldest.value)
      }
    }
    // Native Web/Desktop approval and the device are both live answerers.
    // Never let the native answerer's immediate "unavailable" short-circuit
    // the device; race only meaningful decisions, then let the device timeout
    // remain the fail-closed terminal path.
    const nativeOutcome = Promise.resolve()
      .then(() => (bridge.options.mirrorNative ? delegate() : 'unavailable'))
      .catch(() => 'unavailable')
    const outcome = await new Promise((resolve) => {
      let settled = false
      const finish = (source, value) => {
        if (settled) return
        settled = true
        resolve({ source, value })
      }
      prompt.outcome.then((value) => finish('device', value))
      nativeOutcome.then((value) => {
        if (value === 'allowed-once' || value === 'rejected') finish('native', value)
      })
    })
    if (outcome.source === 'native') {
      bridge.server.cancelApproval(prompt.requestId)
      return outcome.value
    }
    if (outcome.value === 'allowed-once' || outcome.value === 'rejected') return outcome.value
    return 'unavailable'
  } catch (error) {
    bridge.logger.warn(error)
    return 'unavailable'
  }
}

/**
 * Create the bridge: one `BridgeServer` plus every Cordis listener it needs.
 *
 * Listening is intentionally separate (`bridge.start()` / `bridge.stop()`) so
 * tests can drive the transport and the DSH boundary independently.
 *
 * @param {object} ctx - Cordis context, or a structurally compatible fake.
 * @param {object} [config] - Cordis row config (`port`, `approvalTimeoutMs`, ...).
 * @returns {{ctx:object, options:object, logger:object, server:BridgeServer, start:()=>Promise<number>, stop:()=>Promise<void>, handleApprovalRequest:Function, injectToSession:Function, listSessions:Function, resolveTargetSession:Function, summarizeSession:Function}} bridge.
 */
function createBridge(ctx, config = {}) {
  const options = normalizeOptions(config)
  const logger = safeLogger(ctx)
  const bridge = {
    ctx,
    options,
    logger,
    titleMap: new Map(),
    lastModified: new Map(),
    sessionStates: new Map(),
    toolNames: new Map(),
    askedApprovals: new Map(),
    approvalLinks: new Map(),
    server: null,
  }
  bridge.server = new BridgeServer({
    port: options.port,
    approvalTimeoutMs: options.approvalTimeoutMs,
    logger,
    listSessions: () => {
      const sessions = getLiveSessions(ctx)
      const summaries = []
      for (const session of sessions) {
        try {
          summaries.push(summarizeSession(bridge, session))
        } catch (error) {
          logger.warn(error)
        }
      }
      return summaries
    },
    inject: (args) => injectToSession(bridge, args),
    interrupt: (args) => interruptSession(bridge, args),
  })

  // Compact control-plane forwarding to GET /events.
  ctx.on('session/event', (session, event) => {
    try {
      forwardSessionEvent(bridge, session, event)
    } catch (error) {
      logger.warn(error)
    }
  })
  ctx.on('session/created', (session) => {
    try {
      publishSessionSummary(bridge, session)
    } catch (error) {
      logger.warn(error)
    }
  })
  ctx.on('session/disposed', (session) => {
    try {
      if (session && typeof session.id === 'string') {
        bridge.server.publish('session', { session: { id: session.id, removed: true } })
      }
    } catch (error) {
      logger.warn(error)
    }
  })
  ctx.on('agent/status', (payload) => {
    try {
      const session = payload?.agent?.session
      if (session) publishSessionSummary(bridge, session)
    } catch (error) {
      logger.warn(error)
    }
  })

  // The answerer itself: terminal for HachimoDock, delegating by config.
  ctx.on('approval/request', (request, next) => handleApprovalRequest(bridge, request, next))

  bridge.start = async () => bridge.server.listen()
  bridge.stop = async () => bridge.server.close()
  bridge.handleApprovalRequest = (request, next) => handleApprovalRequest(bridge, request, next)
  bridge.interruptSession = (args) => interruptSession(bridge, args)
  bridge.injectToSession = (args) => injectToSession(bridge, args)
  bridge.listSessions = () => bridge.server.listSessions()
  bridge.resolveTargetSession = (sessionId) => resolveTargetSession(bridge, sessionId)
  bridge.summarizeSession = (session) => summarizeSession(bridge, session)
  return bridge
}

/**
 * Cordis plugin body: create the bridge and own its HTTP server lifecycle.
 * @param {object} ctx - Cordis context.
 * @param {object} [config] - Cordis row config.
 * @returns {void}
 */
function apply(ctx, config) {
  const bridge = createBridge(ctx, config)
  ctx.effect(async () => {
    const port = await bridge.start()
    bridge.logger.info(`hachimodock: bridge listening on http://127.0.0.1:${port}`)
    return async () => {
      await bridge.stop()
    }
  }, 'hachimodock: http bridge')
}

module.exports = {
  apply,
  createBridge,
  normalizeOptions,
  AGENT_ID,
  PLUGIN_NAME,
  INJECT,
  injectToSession,
  handleApprovalRequest,
  forwardSessionEvent,
  summarizeSession,
  resolveTargetSession,
}
