'use strict'

/**
 * Shared test doubles for the HachimoDock bridge tests.
 *
 * No DSH packages are imported here: `createFakeCtx` reproduces the small
 * structural surface `dsh-bridge.js` uses (session/agent stores, `ctx.on`,
 * `ctx.get`, logger), so the tests run with `node --test` in any checkout.
 */

const { BridgeServer } = require('../server')
const testServers = new Map()
function headersFor(url) {
  const u = new URL(url), server = testServers.get(u.origin)
  if (!server) return {}
  const role = u.pathname.startsWith('/approval') || u.pathname === '/events' ? 'relay' : u.pathname.startsWith('/voice') ? 'voice' : 'bus'
  return { Authorization: `Bearer ${server.tokens[role]}` }
}

/**
 * Parse one SSE frame block.
 * @param {string} block - text between two blank lines.
 * @returns {{id: string|undefined, event: string, data: unknown}|undefined} parsed frame.
 */
function parseSseBlock(block) {
  let id
  let event = 'message'
  const dataLines = []
  for (const line of block.split('\n')) {
    if (line.startsWith(':')) continue
    const colon = line.indexOf(':')
    const field = colon === -1 ? line : line.slice(0, colon)
    let value = colon === -1 ? '' : line.slice(colon + 1)
    if (value.startsWith(' ')) value = value.slice(1)
    if (field === 'id') id = value
    else if (field === 'event') event = value
    else if (field === 'data') dataLines.push(value)
  }
  if (id === undefined && event === 'message' && dataLines.length === 0) return undefined
  let data
  try {
    data = dataLines.length > 0 ? JSON.parse(dataLines.join('\n')) : undefined
  } catch {
    data = dataLines.join('\n')
  }
  return { id, event, data }
}

/**
 * Consume a streaming `text/event-stream` response into sequential parsed frames.
 * @param {Response} response - fetch response with a readable body.
 * @returns {{events: object[], next: (timeoutMs?: number) => Promise<object>, close: () => void, closed: boolean}} reader.
 */
function createSseReader(response) {
  const events = []
  const waiters = new Set()
  let cursor = 0
  let closed = false
  let failure

  const notify = () => {
    for (const waiter of [...waiters]) waiter()
  }

  const stream = response.body.getReader()
  const pump = (async () => {
    const decoder = new TextDecoder()
    let buffer = ''
    try {
      while (true) {
        const { done, value } = await stream.read()
        if (done) break
        buffer += decoder.decode(value, { stream: true })
        let index
        while ((index = buffer.indexOf('\n\n')) >= 0) {
          const block = buffer.slice(0, index)
          buffer = buffer.slice(index + 2)
          const parsed = parseSseBlock(block)
          if (parsed) events.push(parsed)
        }
        notify()
      }
    } catch (error) {
      failure = error
    } finally {
      closed = true
      notify()
    }
  })()

  /**
   * Wait for the next unconsumed frame.
   * @param {number} [timeoutMs] - wait bound.
   * @returns {Promise<object>} the next frame.
   */
  function next(timeoutMs = 3000) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        cleanup()
        reject(new Error(`timed out waiting for SSE frame (saw ${events.length})`))
      }, timeoutMs)
      if (typeof timer.unref === 'function') timer.unref()
      const check = () => {
        if (cursor < events.length) {
          const frame = events[cursor]
          cursor += 1
          cleanup()
          resolve(frame)
          return
        }
        if (closed) {
          cleanup()
          reject(failure ?? new Error('SSE stream closed before the expected frame'))
        }
      }
      const cleanup = () => {
        clearTimeout(timer)
        waiters.delete(check)
      }
      waiters.add(check)
      check()
    })
  }

  return {
    events,
    next,
    pump,
    close() {
      try {
        stream.cancel().catch(() => {})
      } catch {
        /* ignore */
      }
    },
    get closed() {
      return closed
    },
  }
}

/**
 * Open an SSE endpoint (GET or POST) and return a frame reader.
 * @param {string} url - endpoint URL.
 * @param {object} [options] - fetch options.
 * @returns {Promise<{response: Response, next: Function, events: object[], close: Function}>} live reader.
 */
async function openSse(url, options = {}) {
  const controller = new AbortController()
  const response = await fetch(url, {
    ...options,
    headers: { accept: 'text/event-stream', ...headersFor(url), ...(options.headers ?? {}) },
    signal: controller.signal,
  })
  if (response.status !== 200) {
    throw new Error(`expected 200 for ${url}, got ${response.status}: ${await response.text()}`)
  }
  const reader = createSseReader(response)
  return {
    response,
    events: reader.events,
    next: reader.next,
    close: () => {
      reader.close()
      controller.abort()
    },
  }
}

/**
 * Read a JSON response.
 * @param {string} url - request URL.
 * @param {RequestInit} [options] - fetch options.
 * @returns {Promise<{status:number, body:any}>} status and parsed body.
 */
async function requestJson(url, options = {}) {
  const response = await fetch(url, { ...options, headers: { ...headersFor(url), ...options.headers } })
  const text = await response.text()
  let body
  try {
    body = text === '' ? undefined : JSON.parse(text)
  } catch {
    body = text
  }
  return { status: response.status, body }
}

/**
 * POST one JSON body and return status plus parsed JSON.
 * @param {string} url - request URL.
 * @param {unknown} body - JSON body.
 * @returns {Promise<{status:number, body:any}>} status and parsed body.
 */
function postJson(url, body) {
  const u = new URL(url), server = testServers.get(u.origin)
  if (u.pathname === '/approval/decision' && server && body && typeof body === 'object') {
    const p = server.pending.get(body.requestId) || server.resolved.get(body.requestId)
    if (p) body = { epoch: server.epoch, sessionId: p.sessionId, lease: p.lease, ...body }
  }
  return requestJson(url, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body),
  })
}

/**
 * Wait until a predicate is satisfied (bounded polling).
 * @param {() => boolean} predicate - condition.
 * @param {number} [timeoutMs] - wait bound.
 * @returns {Promise<void>} resolves once true.
 */
async function waitFor(predicate, timeoutMs = 3000) {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (predicate()) return
    await new Promise((resolve) => setTimeout(resolve, 5))
  }
  if (!predicate()) throw new Error('waitFor timed out')
}

/**
 * Create a fake live DSH session with the fields the bridge reads.
 * @param {string} id - session id.
 * @param {object} [extra] - extra header fields.
 * @returns {object} fake session.
 */
function createFakeSession(id, extra = {}) {
  return {
    id,
    seq: 0,
    header: {
      version: 4,
      id,
      createdAt: extra.createdAt ?? Date.now(),
      isSeeded: false,
      ...(extra.cwd === undefined ? {} : { cwd: extra.cwd }),
      ...(extra.header ?? {}),
    },
  }
}

// Mirror Session.append: assign the next seq, commit the increment, then notify.
// Raw dispatch remains available for missing/stale/gap negative controls.
function appendSessionEvent(ctx, session, event) {
  const committed = { ...event, seq: session.seq }
  session.seq += 1
  return ctx.dispatchSync('session/event', session, committed)
}

async function nextControlEvent(sse, expected, sessionId) {
  for (let i = 0; i < 16; i++) {
    const frame = await sse.next()
    if (frame.event === 'session') {
      if (frame.data?.session?.id !== sessionId) throw new Error('unexpected session summary')
      continue
    }
    if (frame.event !== expected) throw new Error(`expected ${expected}, got ${frame.event}`)
    return frame
  }
  throw new Error(`no ${expected} within bounded summary window`)
}

/**
 * Create a fake agent with the documented `followup` / `whenIdle` surface.
 * @param {object} session - owning fake session.
 * @returns {object} fake agent.
 */
function createFakeAgent(session) {
  const agent = {
    id: session.id,
    session,
    status: 'idle',
    followups: [],
    idle: null,
    cancelled: null,
    followup(message) {
      this.followups.push(message)
      this.status = 'running'
      this.idle = Promise.withResolvers()
    },
    cancel(cause) {
      this.cancelled = cause
      this.status = 'idle'
      if (this.idle !== null) {
        this.idle.resolve()
        this.idle = null
      }
    },
    whenIdle() {
      return this.idle === null ? Promise.resolve() : this.idle.promise
    },
    /** Finish the current activity so `whenIdle()` settles. */
    finish() {
      this.status = 'idle'
      if (this.idle !== null) {
        this.idle.resolve()
        this.idle = null
      }
    },
  }
  return agent
}

/**
 * Create a minimal Cordis-compatible context double.
 * @param {object} [options] - initial state and optional services.
 * @param {object[]} [options.sessions] - fake sessions.
 * @param {object[]} [options.agents] - fake agents.
 * @param {object} [options.services] - extra `ctx.get(name)` services.
 * @returns {object} fake context with dispatch helpers.
 */
function createFakeCtx(options = {}) {
  /** @type {Map<string, Function[]>} */
  const handlers = new Map()
  /** @type {Map<string, object>} */
  const sessions = new Map((options.sessions ?? []).map((session) => [session.id, session]))
  /** @type {Map<string, object>} */
  const agents = new Map((options.agents ?? []).map((agent) => [agent.id, agent]))
  const services = { ...(options.services ?? {}) }
  const logged = []

  const ctx = {
    handlers,
    logged,
    sessions: {
      list: () => [...sessions.values()],
      get: (id) => sessions.get(id),
    },
    agents: {
      get: (id) => agents.get(id),
      list: () => [...agents.values()],
    },
    get(name) {
      return services[name]
    },
    provide(name, value) {
      services[name] = value
    },
    on(name, listener) {
      const list = handlers.get(name) ?? []
      list.push(listener)
      handlers.set(name, list)
      return () => {
        const index = list.indexOf(listener)
        if (index >= 0) list.splice(index, 1)
      }
    },
    logger: {
      info: (detail) => logged.push(['info', detail]),
      warn: (detail) => logged.push(['warn', detail]),
      error: (detail) => logged.push(['error', detail]),
    },
    addSession(session) {
      sessions.set(session.id, session)
    },
    removeSession(id) {
      sessions.delete(id)
    },
    addAgent(agent) {
      agents.set(agent.id, agent)
    },
    /** Invoke registered listeners for one event synchronously. */
    dispatchSync(name, ...args) {
      const list = [...(handlers.get(name) ?? [])]
      const results = []
      for (const listener of list) results.push(listener(...args))
      return results
    },
    /** Invoke registered listeners for one event and await them in order. */
    async dispatch(name, ...args) {
      const list = [...(handlers.get(name) ?? [])]
      const results = []
      for (const listener of list) results.push(await listener(...args))
      return results
    },
    /** Call the first listener registered for an event. */
    invokeFirst(name, ...args) {
      const list = handlers.get(name) ?? []
      if (list.length === 0) throw new Error(`no listener registered for ${name}`)
      return list[0](...args)
    },
  }
  return ctx
}

/**
 * Build one fake DSH approval request for the answerer.
 * @param {object} session - fake session the request belongs to.
 * @param {object} [extra] - overrides.
 * @returns {object} fake approval request.
 */
function createApprovalRequest(session, extra = {}) {
  return {
    agent: { id: session.id, session },
    toolName: 'bash',
    reason: 'run the test suite',
    displayReason: 'Run tests?',
    signal: new AbortController().signal,
    ...extra,
  }
}

/**
 * Start a bare `BridgeServer` on an ephemeral port for transport tests.
 * @param {import('node:test').TestContext} t - node:test context for cleanup.
 * @param {object} [options] - `BridgeServer` options.
 * @returns {Promise<{server: BridgeServer, url: string}>} started server and base URL.
 */
async function startTestServer(t, options = {}) {
  const server = new BridgeServer({ port: 0, writeBootstrap: false, ...options })
  await server.listen(0)
  t.after(async () => {
    testServers.delete(server.url())
    await server.close()
  })
  testServers.set(server.url(), server)
  return { server, url: server.url() }
}

/**
 * Start a full fake-DSH bridge on an ephemeral port for integration tests.
 * @param {import('node:test').TestContext} t - node:test context for cleanup.
 * @param {object} ctx - fake context.
 * @param {object} [config] - bridge config overrides.
 * @returns {Promise<{bridge: object, url: string}>} started bridge and base URL.
 */
async function startTestBridge(t, ctx, config = {}) {
  // Unit tests must not inherit the developer's real pet-bridge.json timeout.
  process.env.HACHIMODOCK_BRIDGE_PROFILE = '/nonexistent/hachimodock-test-profile.json'
  const { createBridge } = require('../dsh-bridge')
  const bridge = createBridge(ctx, { port: 0, writeBootstrap: false, ...config })
  await bridge.server.listen(0)
  t.after(async () => {
    testServers.delete(bridge.server.url())
    await bridge.stop()
  })
  testServers.set(bridge.server.url(), bridge.server)
  return { bridge, url: bridge.server.url() }
}

module.exports = {
  appendSessionEvent,
  nextControlEvent,
  parseSseBlock,
  createSseReader,
  openSse,
  requestJson,
  postJson,
  waitFor,
  createFakeSession,
  createFakeAgent,
  createFakeCtx,
  createApprovalRequest,
  startTestServer,
  startTestBridge,
}
