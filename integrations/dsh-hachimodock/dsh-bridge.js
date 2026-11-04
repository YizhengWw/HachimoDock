'use strict'

/** DSH boundary: pinned upstream ce7073d51254c26cd63b22a60835b8f948deb447,
 * 0.2.2-alpha.2 source contracts. Installed-runtime compatibility is NOT tested.
 * Only exact SessionController admission/cancel and a single approval answerer.
 */

const { createStateContract } = require('./dsh-state-contract')
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
const INJECT = ['sessions', 'agents', 'sessionController']

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
  return { port, approvalTimeoutMs, approvalFallback, injectTimeoutMs, maxInjectTokenChars: MAX_INJECT_TOKEN_CHARS, allowedOrigins: options.allowedOrigins || [], bootstrapFile: options.bootstrapFile, writeBootstrap: options.writeBootstrap }
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
 * An explicit live session id is required; auto/newest fallback is forbidden.
 *
 * @param {object} bridge - bridge state.
 * @param {string} sessionId - requested session id or `auto`.
 * @returns {object|undefined} target session.
 */
function resolveTargetSession(bridge, sessionId) {
  if (typeof sessionId !== 'string' || !sessionId || sessionId === 'auto') return undefined
  return getLiveSession(bridge.ctx, sessionId)
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
 * @returns {{id:string,title:string|null,cwd:string|null,lastModified:number,status:'idle'|'running'|'unknown',agentId:string}} summary.
 */
function summarizeSession(bridge, session) {
  let agent
  try {
    agent = bridge.ctx.agents?.get?.(session.id)
    if (agent?.session !== undefined && agent.session !== session) agent = undefined
  } catch {
    agent = undefined
  }
  const snapshot = bridge.stateContract.snapshot(session, bridge.server.epoch)
  // Current registry activity is safe even when attachment missed turn/start.
  // It cannot identify an outstanding question or prove task completion.
  const status = agent?.status === 'running' ? 'running' : agent?.status === 'idle' ? 'idle' : 'unknown'
  const lastModified = bridge.lastModified.get(session.id)
  const createdAt = Number.isSafeInteger(session.header?.createdAt) ? session.header.createdAt : 0
  return {
    id: String(session.id),
    title: titleOf(bridge, session),
    cwd: typeof session.header?.cwd === 'string' ? session.header.cwd : null,
    lastModified: Number.isFinite(lastModified) ? lastModified : createdAt,
    status,
    active: status === 'running' ? true : status === 'idle' ? false : null,
    activitySource: status === 'unknown' ? null : 'live-agent-status',
    stateDetailUnknown: snapshot.state === 'unknown',
    state: snapshot.state,
    waitKind: snapshot.waitKind,
    epoch: snapshot.epoch,
    sourceSeq: snapshot.sourceSeq,
    stateContract: 'live-session-seq-turn-call-v1',
    displayContent: bridge.sessionText.get(String(session.id)) || '',
    transitionRevision: snapshot.revision,
    agentId: AGENT_ID,
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
/**
 * Extract the final assistant text blocks from one `assistant/message` payload.
 * Tool-call/reasoning blocks are intentionally excluded from the device body.
 * @param {object} message - DSH assistant message.
 * @returns {string} bounded plain text, or an empty string.
 */
function assistantMessageText(message) {
  const content = Array.isArray(message?.content) ? message.content : []
  const parts = []
  for (const block of content) {
    if (block && block.type === 'text' && typeof block.text === 'string' && block.text.trim()) {
      parts.push(block.text.trim())
    }
  }
  return boundString(parts.join('\n'), 383) ?? ''
}

function rememberSessionText(bridge, sessionId, text) {
  if (!text) return
  bridge.sessionText.set(sessionId, text)
  if (bridge.sessionText.size > CACHE_LIMIT) {
    const oldest = bridge.sessionText.keys().next()
    if (!oldest.done) bridge.sessionText.delete(oldest.value)
  }
}

function forwardSessionEvent(bridge, session, event) {
  if (!session || typeof session.id !== 'string' || !event || typeof event.type !== 'string') return
  // Reject a detached/replaced Session, even if an older object reuses its id.
  if (getLiveSession(bridge.ctx, session.id) !== session) return
  const normalized = bridge.stateContract.observe(session, event, bridge.server.epoch)
  if (!normalized.accepted) {
    if (normalized.changed) publishSessionSummary(bridge, session)
    return
  }
  const sessionId = session.id
  const time = Number.isSafeInteger(event.time) ? event.time : bridge.server.now()
  remember(bridge.lastModified, sessionId, time)
  const data = event.data !== null && typeof event.data === 'object' ? event.data : {}
  const state = {
    state: normalized.state,
    waitKind: normalized.waitKind,
    epoch: normalized.epoch,
    sourceSeq: normalized.sourceSeq,
    transitionRevision: normalized.revision,
    stateContract: 'live-session-seq-turn-call-v1',
  }
  switch (event.type) {
    case 'session/title': {
      const title = boundString(data.title, 160) ?? ''
      remember(bridge.titleMap, sessionId, title)
      bridge.server.publish('session-title', { sessionId, title, ...state })
      break
    }
    case 'turn/start':
    case 'turn/end':
      bridge.server.publish('turn', {
        sessionId, ...state,
        phase: event.type === 'turn/start' ? 'start' : 'end',
        turn: Number.isSafeInteger(data.turn) ? data.turn : undefined,
        reason: boundString(data.reason?.kind, 64) ?? undefined,
      })
      break
    case 'assistant/message': {
      const text = assistantMessageText(data.message)
      if (text) rememberSessionText(bridge, sessionId, text)
      publishSessionSummary(bridge, session)
      return
    }
    case 'tool/call':
    case 'tool/result': {
      const call = normalized.call
      if (!call) break
      bridge.server.publish('tool', {
        sessionId, ...state,
        phase: event.type === 'tool/call' ? 'start' : 'end',
        name: call.name,
        callId: call.callId,
        ok: event.type === 'tool/call' || (data.message?.isError !== true && !data.error),
      })
      break
    }
    case 'approval/asked':
      bridge.server.publish('approval-asked', {
        sessionId, ...state, approvalId: boundString(data.id, 256) ?? '',
        tool: boundString(data.toolName, 120) ?? 'unknown',
      })
      break
    case 'approval/decided':
      bridge.server.publish('approval-resolved', {
        sessionId, ...state, approvalId: boundString(data.id, 256) ?? '',
        outcome: typeof data.outcome === 'string' ? data.outcome : 'unavailable',
      })
      break
    default:
      if (!normalized.changed) return
  }
  publishSessionSummary(bridge, session)
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
  const target = resolveTargetSession(bridge, args.sessionId)
  if (!target || typeof args.requestId !== 'string' || !args.requestId) {
    args.emit({ type: 'error', code: 'session-not-found', message: 'exact session/request required' }); return
  }
  const controller = bridge.ctx.sessionController
  if (typeof controller?.prompt !== 'function') {
    args.emit({ type: 'error', code: 'session-controller-unavailable', message: 'supported sessionController required' }); return
  }
  try {
    const result = await controller.prompt({ requestId: args.requestId, sessionId: target.id,
      mode: 'queue', content: [{ type: 'text', text: args.text }] }, args.signal)
    if (result?.accepted !== true) throw new Error('prompt not admitted')
    // Admission is not completion of this utterance or the session.
    args.emit({ type: 'done', sessionId: target.id, tokens: 0, stopReason: 'admitted' })
  } catch {
    args.emit({ type: 'error', code: 'delivery-unknown', message: 'prompt admission unavailable or unknown; do not blindly retry' })
  }
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
    if (Number.isSafeInteger(parsed) && parsed >= 0) return parsed === 0 ? NEVER_APPROVAL_TIMEOUT_MS : parsed
  }
  try {
    const profilePath = process.env.HACHIMODOCK_BRIDGE_PROFILE || DEFAULT_BRIDGE_PROFILE
    const raw = JSON.parse(fs.readFileSync(profilePath, 'utf8'))
    const value = raw?.dshApprovalTimeoutMs
    if (Number.isSafeInteger(value) && value >= 0) return value === 0 ? NEVER_APPROVAL_TIMEOUT_MS : value
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
  if (typeof bridge.ctx.sessionController?.cancel !== 'function') return { ok: false, error: 'session-controller-unavailable' }
  const receipt = await bridge.ctx.sessionController.cancel({ sessionId: target.id })
  return receipt?.accepted === true ? { ok: true, sessionId: target.id, interrupted: true, state: 'cancel-requested' } : { ok: false, error: 'interrupt-failed' }
}

async function handleApprovalRequest(bridge, request, next) {
  const delegate = typeof next === 'function' ? next : () => Promise.resolve('unavailable')
  try {
    const sessionId = request?.agent?.session?.id
    if (typeof sessionId !== 'string') return 'unavailable'
    if (request.signal?.aborted) return 'cancelled'
    if (getLiveSession(bridge.ctx, sessionId) !== request.agent.session) return 'unavailable'
    if (!bridge.server.hasEventSubscriber()) {
      if (bridge.options.approvalFallback === 'delegate') return await delegate()
      return 'unavailable'
    }
    const prompt = bridge.server.requestApproval({
      agentId: AGENT_ID, sessionId,
      tool: typeof request.toolName === 'string' ? request.toolName : 'unknown',
      reason: typeof request.reason === 'string' ? request.reason : undefined,
      displayReason: typeof request.displayReason === 'string' ? request.displayReason
        : typeof request.displayReason?.['zh-CN'] === 'string' ? request.displayReason['zh-CN']
        : typeof request.displayReason?.zh === 'string' ? request.displayReason.zh
        : typeof request.displayReason?.en === 'string' ? request.displayReason.en : undefined,
      timeoutMs: resolveApprovalTimeoutMs(bridge), signal: request.signal,
    })
    // Claim once. No parallel next(), no DOM dismissal and no reassignment on disconnect.
    return await prompt.outcome
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
    stateContract: createStateContract({ limit: CACHE_LIMIT }),
    sessionText: new Map(),
    server: null,
  }
  bridge.server = new BridgeServer({
    port: options.port,
    approvalTimeoutMs: options.approvalTimeoutMs,
    injectTimeoutMs: options.injectTimeoutMs,
    logger,
    allowedOrigins: options.allowedOrigins,
    bootstrapFile: options.bootstrapFile,
    writeBootstrap: options.writeBootstrap,
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
        const current = getLiveSession(bridge.ctx, session.id)
        if (current && current !== session) return
        bridge.stateContract.forget(session)
        bridge.server.retireSession(session.id)
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
