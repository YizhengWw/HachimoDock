'use strict'

/**
 * Transport and state for the HachimoDock <-> DeepSeek Harness bridge.
 *
 * This module knows nothing about Cordis or DSH. It owns:
 *   - one loopback `node:http` server (`127.0.0.1` only),
 *   - the `/health`, `/sessions`, `/inject`, `/events`, `/approval/decision`
 *     contract endpoints from `docs/dsh-integration-contract.md`,
 *   - the bounded event ring buffer behind `GET /events?after=<seq>`,
 *   - pending approval requests and their idempotent one-shot decisions.
 *
 * Everything DSH-specific is injected through `listSessions` and `inject`, so
 * `test/` can exercise the wire contract with fake dependencies and no DSH
 * profile. `dsh-bridge.js` supplies the real implementations.
 *
 * Wire shapes (protocol version 1):
 *   GET  /health             -> {"ok":true,"version":1,"sessions":N,"pendingApprovals":N}
 *   GET  /sessions           -> {"sessions":[{id,title,cwd,lastModified,status,agentId}]}
 *   POST /inject             -> text/event-stream: token | tool | done | error
 *   GET  /events?after=<seq> -> text/event-stream: id/event/data frames
 *   POST /approval/decision  -> {"ok":true,"requestId":...,"accepted":true} | 400 | 409
 *
 * @module @hachimodock/dsh-bridge/server
 */

const { createServer } = require('node:http')
const { randomUUID } = require('node:crypto')

/** Protocol version advertised by `GET /health`. */
const PROTOCOL_VERSION = 1
/** Default localhost port from the contract. */
const DEFAULT_PORT = 17892
/** Default approval wait before failing closed. */
const DEFAULT_APPROVAL_TIMEOUT_MS = 30000
/** Sentinel used by the UI for 'never'; still a concrete large timeout. */
const NEVER_APPROVAL_TIMEOUT_MS = 99999
/** Largest accepted JSON request body (contract: body limit 64KB). */
const MAX_BODY_BYTES = 64 * 1024
/** Hard cap on `/inject` text after JSON decoding. */
const MAX_INJECT_TEXT_CHARS = 32 * 1024
/** Bounded replay window for `/events?after=<seq>`. */
const EVENT_BUFFER_LIMIT = 512
/** How many terminal approval ids stay queryable for duplicate detection. */
const RESOLVED_APPROVAL_LIMIT = 256
/** Comment interval keeping idle SSE connections alive. */
const SSE_KEEPALIVE_MS = 15000
/** Longest single field mirrored from DSH state into an event. */
const MAX_EVENT_STRING = 200

const LOOPBACK_ADDRESSES = new Set(['127.0.0.1', '::1', '::ffff:127.0.0.1'])

/**
 * Whether a peer socket address is a loopback literal.
 * @param {string|undefined} address - `req.socket.remoteAddress`.
 * @returns {boolean} true for 127.0.0.1 and IPv6 loopback forms.
 */
function isLoopbackAddress(address) {
  if (typeof address !== 'string') return false
  return LOOPBACK_ADDRESSES.has(address)
}

/**
 * Truncate one string field so event payloads stay bounded.
 * @param {unknown} value - candidate value.
 * @param {number} [max] - maximum retained characters.
 * @returns {string|undefined} the string, or undefined when not a string.
 */
function boundString(value, max = MAX_EVENT_STRING) {
  if (typeof value !== 'string') return undefined
  return value.length <= max ? value : `${value.slice(0, max - 1)}…`
}

/**
 * Error carrying an HTTP status and stable machine code for a JSON response.
 * @param {number} status - HTTP status.
 * @param {string} code - stable machine-readable error code.
 * @param {string} [message] - optional human-readable detail.
 * @returns {Error & {status:number, code:string}} tagged error.
 */
function httpError(status, code, message) {
  const error = new Error(message ?? code)
  error.status = status
  error.code = code
  return error
}

/**
 * Write one JSON response.
 * @param {import('node:http').ServerResponse} res - response to finish.
 * @param {number} status - HTTP status.
 * @param {unknown} body - JSON-serializable body.
 * @returns {void}
 */
function sendJson(res, status, body) {
  const text = JSON.stringify(body)
  res.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': Buffer.byteLength(text),
    'cache-control': 'no-store',
    'access-control-allow-origin': '*',
  })
  res.end(text)
}

/**
 * Send SSE headers and flush them so a client's fetch resolves immediately.
 * @param {import('node:http').ServerResponse} res - response to turn into a stream.
 * @returns {void}
 */
function sendSseHeaders(res) {
  res.writeHead(200, {
    'content-type': 'text/event-stream; charset=utf-8',
    'cache-control': 'no-cache, no-transform',
    connection: 'keep-alive',
    'x-accel-buffering': 'no',
    'access-control-allow-origin': '*',
  })
  res.flushHeaders()
}

/**
 * Write one SSE frame.
 * @param {import('node:http').ServerResponse} res - target response.
 * @param {{id?: number|string, event?: string, data?: unknown}} frame - frame content.
 * @returns {boolean} false once the response is no longer writable.
 */
function writeSseFrame(res, frame) {
  if (res.writableEnded || res.destroyed) return false
  let text = ''
  if (frame.id !== undefined) text += `id: ${frame.id}\n`
  if (frame.event !== undefined) text += `event: ${frame.event}\n`
  if (frame.data !== undefined) text += `data: ${JSON.stringify(frame.data)}\n`
  text += '\n'
  try {
    res.write(text)
    return true
  } catch {
    return false
  }
}

/**
 * Write one SSE comment (used for keep-alive and connection acknowledgement).
 * @param {import('node:http').ServerResponse} res - target response.
 * @param {string} text - comment text.
 * @returns {boolean} false once the response is no longer writable.
 */
function writeSseComment(res, text) {
  if (res.writableEnded || res.destroyed) return false
  try {
    res.write(`: ${text}\n\n`)
    return true
  } catch {
    return false
  }
}

/**
 * Read a bounded JSON request body.
 * @param {import('node:http').IncomingMessage} req - request to read.
 * @param {number} [limit] - maximum body bytes.
 * @returns {Promise<unknown>} parsed JSON value.
 * @throws {Error & {status:number, code:string}} 400 invalid-json / empty, 413 payload-too-large.
 */
function readJsonBody(req, limit = MAX_BODY_BYTES) {
  return new Promise((resolve, reject) => {
    const declared = Number(req.headers['content-length'])
    if (Number.isFinite(declared) && declared > limit) {
      reject(httpError(413, 'payload-too-large', `request body exceeds ${limit} bytes`))
      return
    }
    const chunks = []
    let size = 0
    let settled = false
    const fail = (error) => {
      if (settled) return
      settled = true
      reject(error)
    }
    req.on('data', (chunk) => {
      if (settled) return
      size += chunk.length
      if (size > limit) {
        fail(httpError(413, 'payload-too-large', `request body exceeds ${limit} bytes`))
        return
      }
      chunks.push(chunk)
    })
    req.on('end', () => {
      if (settled) return
      settled = true
      const text = Buffer.concat(chunks).toString('utf8').trim()
      if (text === '') {
        reject(httpError(400, 'invalid-json', 'request body must be JSON'))
        return
      }
      try {
        resolve(JSON.parse(text))
      } catch {
        reject(httpError(400, 'invalid-json', 'request body is not valid JSON'))
      }
    })
    req.on('error', (error) => fail(httpError(400, 'invalid-json', error.message)))
  })
}

/**
 * Bridge transport: HTTP server, event buffer, SSE subscribers, approvals.
 */
class BridgeServer {
  /**
   * @param {object} [options] - injectable dependencies and bounds.
   * @param {number} [options.port] - listen port (0 picks a free one in tests).
   * @param {number} [options.approvalTimeoutMs] - default approval wait.
   * @param {() => Array<object>} [options.listSessions] - compact session summaries.
   * @param {(args: {sessionId: string, text: string, emit: (event: object) => void, signal: AbortSignal}) => Promise<void>} [options.inject] - prompt delivery.
   * @param {(args: {sessionId: string}) => Promise<object>} [options.interrupt] - cancel the active turn.
   * @param {{info?: Function, warn?: Function, error?: Function}} [options.logger] - optional logger.
   * @param {() => number} [options.now] - clock.
   * @param {() => string} [options.createRequestId] - approval id factory.
   */
  constructor(options = {}) {
    this.port = options.port ?? DEFAULT_PORT
    this.host = '127.0.0.1'
    this.approvalTimeoutMs = options.approvalTimeoutMs ?? DEFAULT_APPROVAL_TIMEOUT_MS
    this.listSessions = options.listSessions ?? (() => [])
    this.inject =
      options.inject ??
      (async ({ emit }) => {
        emit({ type: 'error', code: 'inject-unavailable', message: 'no inject adapter configured' })
      })
    this.interrupt =
      options.interrupt ??
      (async () => ({ ok: false, error: 'interrupt-unavailable' }))
    this.logger = options.logger ?? null
    this.now = options.now ?? (() => Date.now())
    this.createRequestId = options.createRequestId ?? (() => `req-${randomUUID()}`)

    /** @type {import('node:http').Server|null} */
    this.httpServer = null
    /** @type {number|null} */
    this.listeningPort = null

    // Bounded replay ring: oldest first, entries are the exact broadcast payloads.
    /** @type {Array<{seq:number, type:string, [key:string]: unknown}>} */
    this.events = []
    this.nextSeq = 1
    /** @type {Set<{res: import('node:http').ServerResponse, ended: boolean}>} */
    this.subscribers = new Set()

    /** @type {Map<string, {requestId:string, resolve:(outcome:string)=>void, timer:NodeJS.Timeout|null, removeAbort:Function|null, state:'pending'}>} */
    this.pending = new Map()
    /** @type {Map<string, {outcome:string, at:number, expired:boolean}>} */
    this.resolved = new Map()
  }

  /**
   * Start listening. Idempotent.
   * @param {number} [port] - override port (0 for an ephemeral test port).
   * @returns {Promise<number>} the actual listening port.
   */
  async listen(port) {
    if (this.httpServer) return this.listeningPort
    const server = createServer((req, res) => {
      this.handleRequest(req, res).catch((error) => this.failRequest(res, error))
    })
    server.headersTimeout = 10000
    server.requestTimeout = 15000
    server.keepAliveTimeout = 5000
    await new Promise((resolve, reject) => {
      const onError = (error) => {
        server.off('error', onError)
        reject(error)
      }
      server.once('error', onError)
      server.listen(port ?? this.port, this.host, () => {
        server.off('error', onError)
        server.on('error', (error) => this.log('error', error))
        resolve()
      })
    })
    this.httpServer = server
    const address = server.address()
    this.listeningPort = typeof address === 'object' && address !== null ? address.port : null
    return this.listeningPort
  }

  /**
   * Base URL of the running server.
   * @returns {string} loopback base URL.
   */
  url() {
    return `http://127.0.0.1:${this.listeningPort ?? this.port}`
  }

  /**
   * Stop listening, end SSE connections, and fail pending approvals closed.
   * Idempotent.
   * @returns {Promise<void>} settles after the listener is closed.
   */
  async close() {
    for (const requestId of [...this.pending.keys()]) this.settleApproval(requestId, 'unavailable', { expired: true })
    for (const subscriber of [...this.subscribers]) this.closeSubscriber(subscriber)
    this.subscribers.clear()
    const server = this.httpServer
    this.httpServer = null
    this.listeningPort = null
    if (!server) return
    await new Promise((resolve) => {
      server.close(() => resolve())
      if (typeof server.closeAllConnections === 'function') server.closeAllConnections()
    })
  }

  /**
   * Log through the injected logger without letting logger failures escape.
   * @param {'info'|'warn'|'error'} level - log level.
   * @param {unknown} detail - error or message.
   * @returns {void}
   */
  log(level, detail) {
    try {
      this.logger?.[level]?.(detail)
    } catch {
      /* a logger must never break the bridge */
    }
  }

  /**
   * Publish one event to the replay buffer and every `/events` subscriber.
   * @param {string} type - SSE event name, mirrored into `data.type`.
   * @param {object} [payload] - event fields (bounded by the callers).
   * @returns {{seq:number, type:string, [key:string]: unknown}} the broadcast event.
   */
  publish(type, payload = {}) {
    const event = { ...payload, seq: this.nextSeq, type }
    this.nextSeq += 1
    this.events.push(event)
    if (this.events.length > EVENT_BUFFER_LIMIT) this.events.splice(0, this.events.length - EVENT_BUFFER_LIMIT)
    for (const subscriber of this.subscribers) this.writeToSubscriber(subscriber, event)
    return event
  }

  /**
   * @returns {boolean} whether at least one HachimoDock `/events` consumer is attached.
   */
  hasEventSubscriber() {
    for (const subscriber of this.subscribers) {
      if (subscriber.role !== 'client') return true
    }
    return false
  }

  /**
   * Broadcast an approval prompt and wait for its one-shot decision.
   *
   * The returned promise always settles with `allowed-once`, `rejected`, or
   * `unavailable` (timeout, abort, or bridge shutdown). A request id that was
   * already resolved here never resolves twice.
   *
   * @param {object} request - approval context.
   * @param {string} request.agentId - agent display id (`deepseek-harness`).
   * @param {string} request.sessionId - DSH session id.
   * @param {string} request.tool - tool name awaiting approval.
   * @param {string} [request.reason] - audited reason.
   * @param {string} [request.displayReason] - localized display text.
   * @param {string} [request.approvalId] - DSH audit id when it could be correlated.
   * @param {number} [request.timeoutMs] - override wait before failing closed.
   * @param {AbortSignal} [request.signal] - DSH request cancellation signal.
   * @returns {{requestId: string, expiresAtMs: number, outcome: Promise<string>}} prompt handle.
   */
  requestApproval(request) {
    const requestId = this.createRequestId()
    const timeoutMs =
      Number.isSafeInteger(request.timeoutMs) && request.timeoutMs > 0
        ? request.timeoutMs
        : this.approvalTimeoutMs
    const neverExpires = timeoutMs === NEVER_APPROVAL_TIMEOUT_MS
    const expiresAtMs = neverExpires ? 0 : this.now() + timeoutMs
    /** @type {Promise<string>} */
    const outcome = new Promise((resolve) => {
      const pending = {
        requestId,
        resolve,
        timer: null,
        removeAbort: null,
      }
      // Register before arming the timer/abort path so an already-aborted
      // signal or an immediate timeout can settle this exact request.
      this.pending.set(requestId, pending)
      if (!neverExpires) {
        pending.timer = setTimeout(() => {
          if (this.settleApproval(requestId, 'unavailable', { expired: true })) {
            this.publish('approval-expired', { requestId })
          }
        }, timeoutMs)
        if (typeof pending.timer.unref === 'function') pending.timer.unref()
      }
      if (request.signal) {
        const onAbort = () => {
          if (this.settleApproval(requestId, 'unavailable', { expired: false })) {
            this.publish('approval-cancel', { requestId })
          }
        }
        if (request.signal.aborted) {
          onAbort()
        } else {
          request.signal.addEventListener('abort', onAbort, { once: true })
          pending.removeAbort = () => request.signal.removeEventListener('abort', onAbort)
        }
      }
    })
    this.publish('approval', {
      requestId,
      agentId: boundString(request.agentId, 80) ?? 'deepseek-harness',
      sessionId: boundString(request.sessionId, 200) ?? '',
      tool: boundString(request.tool, 120) ?? 'unknown',
      reason: boundString(request.reason) ?? '',
      displayReason: boundString(request.displayReason) ?? '',
      ...(typeof request.approvalId === 'string' ? { approvalId: boundString(request.approvalId) } : {}),
      expiresAtMs,
      timeoutMs,
      neverExpires,
    })
    return { requestId, expiresAtMs, outcome }
  }

  /**
   * Apply one decision to a pending approval.
   *
   * A pending id is consumed exactly once. A duplicate decision for an already
   * decided id is a conflict (`already-decided`); an id that timed out, was
   * aborted, or never existed is `unknown-or-expired`.
   *
   * @param {string} requestId - id from the prompt broadcast.
   * @param {'allow'|'deny'} decision - device decision.
   * @returns {{ok: true, requestId: string, accepted: true} | {ok: false, requestId?: string, error: string}} result.
   */
  decideApproval(requestId, decision) {
    const pending = this.pending.get(requestId)
    if (pending !== undefined) {
      const outcome = decision === 'allow' ? 'allowed-once' : 'rejected'
      this.settleApproval(requestId, outcome, { expired: false })
      return { ok: true, requestId, accepted: true }
    }
    const resolved = this.resolved.get(requestId)
    if (resolved !== undefined) {
      if (resolved.expired) return { ok: false, requestId, error: 'unknown-or-expired' }
      return { ok: false, requestId, error: 'already-decided' }
    }
    return { ok: false, requestId, error: 'unknown-or-expired' }
  }

  /**
   * Withdraw a pending approval because the native DSH answerer won. This emits
   * an approval-cancel event so the HachimoDock relay can clear the device modal.
   * @param {string} requestId - pending approval id.
   * @returns {boolean} whether a pending request was cancelled.
   */
  cancelApproval(requestId, outcome = 'unavailable') {
    if (!this.pending.has(requestId)) return false
    this.settleApproval(requestId, 'unavailable', { expired: false })
    this.publish('approval-cancel', { requestId, outcome })
    return true
  }

  /**
   * Settle one pending approval, remember its terminal state, and clean up.
   * @param {string} requestId - pending id.
   * @param {string} outcome - closed approval outcome.
   * @param {{expired: boolean}} flags - whether the terminal state is an expiry.
   * @returns {boolean} whether a pending request was settled.
   */
  settleApproval(requestId, outcome, flags) {
    const pending = this.pending.get(requestId)
    if (pending === undefined) return false
    this.pending.delete(requestId)
    if (pending.timer !== null) clearTimeout(pending.timer)
    if (typeof pending.removeAbort === 'function') {
      try {
        pending.removeAbort()
      } catch {
        /* ignore */
      }
    }
    this.resolved.set(requestId, { outcome, at: this.now(), expired: flags.expired })
    if (this.resolved.size > RESOLVED_APPROVAL_LIMIT) {
      const oldest = this.resolved.keys().next()
      if (!oldest.done) this.resolved.delete(oldest.value)
    }
    pending.resolve(outcome)
    return true
  }

  /**
   * Write one buffered event to one subscriber.
   * @param {{res: import('node:http').ServerResponse, ended: boolean}} subscriber - SSE consumer.
   * @param {{seq:number, type:string}} event - event to write.
   * @returns {void}
   */
  writeToSubscriber(subscriber, event) {
    const ok = writeSseFrame(subscriber.res, { id: event.seq, event: event.type, data: event })
    if (!ok) this.closeSubscriber(subscriber)
  }

  /**
   * End and forget one subscriber.
   * @param {{res: import('node:http').ServerResponse, ended: boolean}} subscriber - SSE consumer.
   * @returns {void}
   */
  closeSubscriber(subscriber) {
    if (subscriber.ended) return
    subscriber.ended = true
    this.subscribers.delete(subscriber)
    try {
      subscriber.res.end()
    } catch {
      /* ignore */
    }
  }

  /**
   * Route one HTTP request. Unexpected failures answer 500 when headers are
   * still unsent and simply end the response otherwise.
   * @param {import('node:http').IncomingMessage} req - request.
   * @param {import('node:http').ServerResponse} res - response.
   * @returns {Promise<void>} settles after the response is handed off.
   */
  async handleRequest(req, res) {
    if (!isLoopbackAddress(req.socket.remoteAddress)) {
      sendJson(res, 403, { ok: false, error: 'forbidden' })
      return
    }
    let url
    try {
      url = new URL(req.url ?? '/', 'http://127.0.0.1')
    } catch {
      sendJson(res, 400, { ok: false, error: 'invalid-request' })
      return
    }
    const path = url.pathname
    const method = req.method ?? 'GET'

    if (path === '/health') {
      if (method !== 'GET') return sendJson(res, 405, { ok: false, error: 'method-not-allowed' })
      return this.handleHealth(res)
    }
    if (path === '/sessions') {
      if (method !== 'GET') return sendJson(res, 405, { ok: false, error: 'method-not-allowed' })
      return this.handleSessions(res)
    }
    if (path === '/inject') {
      if (method !== 'POST') return sendJson(res, 405, { ok: false, error: 'method-not-allowed' })
      return this.handleInject(req, res)
    }
    if (path === '/interrupt') {
      if (method !== 'POST') return sendJson(res, 405, { ok: false, error: 'method-not-allowed' })
      return this.handleInterrupt(req, res)
    }
    if (path === '/events') {
      if (method !== 'GET') return sendJson(res, 405, { ok: false, error: 'method-not-allowed' })
      return this.handleEvents(req, res, url)
    }
    if (path === '/voice/draft' || path === '/voice/submit') {
      if (method !== 'POST') return sendJson(res, 405, { ok: false, error: 'method-not-allowed' })
      const body = await readJsonBody(req).catch(() => ({}))
      const text = typeof body?.text === 'string' ? body.text.slice(0, 4000) : ''
      if (path === '/voice/draft') {
        if (!text.trim()) return sendJson(res, 400, { ok: false, error: 'invalid-text' })
        this.publish('voice-insert', { text })
      } else {
        this.publish('voice-submit', { sessionId: typeof body?.sessionId === 'string' ? body.sessionId : '' })
      }
      return sendJson(res, 200, { ok: true })
    }
    if (path === '/approval/decision') {
      if (method !== 'POST') return sendJson(res, 405, { ok: false, error: 'method-not-allowed' })
      return this.handleApprovalDecision(req, res)
    }
    sendJson(res, 404, { ok: false, error: 'not-found' })
  }

  /**
   * Answer unexpected route failures.
   * @param {import('node:http').ServerResponse} res - response.
   * @param {unknown} error - failure.
   * @returns {void}
   */
  failRequest(res, error) {
    this.log('error', error)
    if (res.headersSent) {
      try {
        res.end()
      } catch {
        /* ignore */
      }
      return
    }
    if (error && typeof error === 'object' && Number.isSafeInteger(error.status)) {
      sendJson(res, error.status, { ok: false, error: error.code ?? 'invalid-request', message: String(error.message) })
      return
    }
    sendJson(res, 500, { ok: false, error: 'internal', message: 'bridge request failed' })
  }

  /**
   * GET /health.
   * @param {import('node:http').ServerResponse} res - response.
   * @returns {void}
   */
  handleHealth(res) {
    let sessions = 0
    try {
      sessions = this.listSessions().length
    } catch (error) {
      this.log('warn', error)
    }
    sendJson(res, 200, {
      ok: true,
      version: PROTOCOL_VERSION,
      sessions,
      pendingApprovals: this.pending.size,
    })
  }

  /**
   * GET /sessions.
   * @param {import('node:http').ServerResponse} res - response.
   * @returns {void}
   */
  handleSessions(res) {
    let sessions
    try {
      sessions = this.listSessions()
    } catch (error) {
      this.log('warn', error)
      sendJson(res, 500, { ok: false, error: 'internal' })
      return
    }
    if (!Array.isArray(sessions)) sessions = []
    sendJson(res, 200, { sessions: sessions.slice(0, 128) })
  }

  /**
   * POST /interrupt: cancel the current DSH turn without streaming a response.
   * @param {import('node:http').IncomingMessage} req - request.
   * @param {import('node:http').ServerResponse} res - response.
   * @returns {Promise<void>} settles after the response.
   */
  async handleInterrupt(req, res) {
    let body
    try {
      body = await readJsonBody(req)
    } catch (error) {
      this.failRequest(res, error)
      return
    }
    const sessionId = typeof body.sessionId === 'string' ? body.sessionId.trim() : ''
    try {
      const result = await this.interrupt({ sessionId })
      if (!result || result.ok !== true) {
        sendJson(res, 409, {
          ok: false,
          error: result?.error === 'session-not-found' ? 'session-not-found' : 'interrupt-failed',
          message: String(result?.error ?? 'interrupt failed'),
        })
        return
      }
      sendJson(res, 200, result)
    } catch (error) {
      sendJson(res, 500, { ok: false, error: 'interrupt-failed', message: String(error?.message || error) })
    }
  }

  /**
   * POST /inject: validate the prompt, then stream the delivery over SSE.
   * @param {import('node:http').IncomingMessage} req - request.
   * @param {import('node:http').ServerResponse} res - response.
   * @returns {Promise<void>} settles after the injection finishes.
   */
  async handleInject(req, res) {
    let body
    try {
      body = await readJsonBody(req)
    } catch (error) {
      this.failRequest(res, error)
      return
    }
    if (body === null || typeof body !== 'object' || Array.isArray(body)) {
      sendJson(res, 400, { ok: false, error: 'invalid-request', message: 'body must be a JSON object' })
      return
    }
    const sessionId =
      typeof body.sessionId === 'string' && body.sessionId.trim() !== '' ? body.sessionId.trim() : 'auto'
    const text = typeof body.text === 'string' ? body.text : ''
    if (text.trim() === '') {
      sendJson(res, 400, { ok: false, error: 'invalid-request', message: 'text must be a non-empty string' })
      return
    }
    if (text.length > MAX_INJECT_TEXT_CHARS) {
      sendJson(res, 400, {
        ok: false,
        error: 'invalid-request',
        message: `text exceeds ${MAX_INJECT_TEXT_CHARS} characters`,
      })
      return
    }

    sendSseHeaders(res)
    const controller = new AbortController()
    let finished = false
    let errored = false
    let doneSent = false
    const keepAlive = setInterval(() => writeSseComment(res, 'keep-alive'), SSE_KEEPALIVE_MS)
    if (typeof keepAlive.unref === 'function') keepAlive.unref()
    const stop = () => {
      if (finished) return
      finished = true
      clearInterval(keepAlive)
      try {
        res.end()
      } catch {
        /* ignore */
      }
    }
    res.on('close', () => {
      controller.abort()
      finished = true
      clearInterval(keepAlive)
    })
    /**
     * Emit one structured inject event to the HachimoDock PC bridge.
     * @param {object} event - adapter event (`token`, `tool`, `done`, `error`).
     * @returns {void}
     */
    const emit = (event) => {
      if (finished || !event || typeof event !== 'object') return
      switch (event.type) {
        case 'token':
          if (typeof event.text !== 'string' || event.text === '') return
          writeSseFrame(res, { event: 'token', data: { text: boundString(event.text, 4096) } })
          return
        case 'tool':
          writeSseFrame(res, {
            event: 'tool',
            data: {
              name: boundString(event.name, 120) ?? 'tool',
              phase: event.phase === 'end' ? 'end' : 'start',
              ok: event.ok !== false,
            },
          })
          return
        case 'done':
          doneSent = true
          writeSseFrame(res, {
            event: 'done',
            data: {
              sessionId: boundString(event.sessionId, 200) ?? sessionId,
              tokens: Number.isSafeInteger(event.tokens) && event.tokens >= 0 ? event.tokens : 0,
            },
          })
          stop()
          return
        case 'error':
          errored = true
          writeSseFrame(res, {
            event: 'error',
            data: {
              code: boundString(event.code, 80) ?? 'inject-failed',
              message: boundString(event.message, 500) ?? 'inject failed',
            },
          })
          stop()
          return
        default:
          return
      }
    }
    writeSseComment(res, 'connected')
    try {
      await this.inject({ sessionId, text, emit, signal: controller.signal })
    } catch (error) {
      this.log('warn', error)
      if (!errored) {
        errored = true
        emit({ type: 'error', code: 'inject-failed', message: errorMessage(error) })
      }
    } finally {
      if (!doneSent && !errored && !finished) emit({ type: 'done', sessionId, tokens: 0 })
      stop()
    }
  }

  /**
   * GET /events?after=<seq>: replay the retained window, then stream live events.
   * @param {import('node:http').IncomingMessage} req - request.
   * @param {import('node:http').ServerResponse} res - response.
   * @param {URL} url - parsed request URL.
   * @returns {void}
   */
  handleEvents(req, res, url) {
    const afterParam = url.searchParams.get('after')
    let after = null
    if (afterParam !== null) {
      const parsed = Number(afterParam)
      if (!Number.isSafeInteger(parsed) || parsed < 0) {
        sendJson(res, 400, { ok: false, error: 'invalid-after', message: 'after must be a non-negative integer' })
        return
      }
      after = parsed
    }
    sendSseHeaders(res)
    const subscriber = {
      res,
      ended: false,
      role: url.searchParams.get('role') === 'client' ? 'client' : 'bridge',
    }
    this.subscribers.add(subscriber)
    const keepAlive = setInterval(() => writeSseComment(res, 'keep-alive'), SSE_KEEPALIVE_MS)
    if (typeof keepAlive.unref === 'function') keepAlive.unref()
    res.on('close', () => {
      clearInterval(keepAlive)
      subscriber.ended = true
      this.subscribers.delete(subscriber)
    })
    try {
      if (after !== null) {
        for (const event of this.events) {
          if (event.seq > after) this.writeToSubscriber(subscriber, event)
        }
      }
      writeSseComment(res, 'connected')
    } catch (error) {
      this.log('warn', error)
      this.closeSubscriber(subscriber)
      clearInterval(keepAlive)
    }
  }

  /**
   * POST /approval/decision.
   * @param {import('node:http').IncomingMessage} req - request.
   * @param {import('node:http').ServerResponse} res - response.
   * @returns {Promise<void>} settles after the response is written.
   */
  async handleApprovalDecision(req, res) {
    let body
    try {
      body = await readJsonBody(req)
    } catch (error) {
      this.failRequest(res, error)
      return
    }
    if (body === null || typeof body !== 'object' || Array.isArray(body)) {
      sendJson(res, 400, { ok: false, error: 'invalid-request', message: 'body must be a JSON object' })
      return
    }
    const requestId = typeof body.requestId === 'string' ? body.requestId.trim() : ''
    const decision = body.decision
    if (requestId === '' || (decision !== 'allow' && decision !== 'deny')) {
      sendJson(res, 400, {
        ok: false,
        error: 'invalid-request',
        message: 'requestId must be a string and decision must be "allow" or "deny"',
      })
      return
    }
    const result = this.decideApproval(requestId, decision)
    if (result.ok) {
      sendJson(res, 200, result)
      return
    }
    sendJson(res, 409, result)
  }
}

/**
 * Normalize one failure into a bounded message.
 * @param {unknown} error - failure value.
 * @returns {string} message text.
 */
function errorMessage(error) {
  if (error instanceof Error) return error.message
  return String(error)
}

module.exports = {
  BridgeServer,
  PROTOCOL_VERSION,
  DEFAULT_PORT,
  DEFAULT_APPROVAL_TIMEOUT_MS,
  MAX_BODY_BYTES,
  EVENT_BUFFER_LIMIT,
  isLoopbackAddress,
  readJsonBody,
  boundString,
}
