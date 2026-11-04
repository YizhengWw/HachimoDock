'use strict'

// Structural contracts, never Desktop version/hash gates. Unknown shapes are
// unsupported until a separately validated adapter exists.
function serviceView(ctx) {
  const optional = name => {
    try { return typeof ctx.get === 'function' ? ctx.get(name) : undefined }
    catch { return undefined }
  }
  return {
    get sessions() { return ctx.sessions },
    get on() { return ctx.on },
    get agents() { return optional('agents') },
    get sessionController() { return optional('sessionController') },
    get clientModules() { return optional('clientModules') },
    get(name) { return ['sessionTitle', 'sessionProjections'].includes(name) ? optional(name) : undefined },
  }
}
function capabilities(ctx) {
  let sessions = false
  try {
    const rows = typeof ctx.sessions?.list === 'function' ? ctx.sessions.list() : null
    sessions = typeof ctx.sessions?.get === 'function' && Array.isArray(rows)
      && rows.every(s => s && typeof s.id === 'string' && s.id.length > 0 && s.id.length <= 200)
  } catch { /* retain unsupported */ }
  let agentSnapshot = false, clientBundle = false
  try { agentSnapshot = sessions && typeof ctx.agents?.get === 'function' } catch { /* absent */ }
  try {
    const graph = typeof ctx.clientModules?.graph === 'function' ? ctx.clientModules.graph() : null
    clientBundle = Array.isArray(graph?.entries) && graph.entries.some(e => e?.id === '@hachimodock/dsh-bridge'
      && typeof e.url === 'string' && e.immediately === true && Array.isArray(e.inject)
      && e.inject.includes('@deepseek-ai/dsh-client-ui-conversation'))
  } catch { /* bundle availability is independent of host and scoped owner */ }
  let backgroundPrompt = false, interrupt = false
  try {
    backgroundPrompt = sessions && typeof ctx.sessionController?.prompt === 'function'
    interrupt = sessions && typeof ctx.sessionController?.cancel === 'function'
  } catch { /* optional control fiber is not active */ }
  return Object.freeze({
    adapter: 'cordis-session-store-v1', sessions, agentSnapshot, clientBundle, backgroundPrompt, interrupt,
    sessionList: sessions, liveState: sessions && typeof ctx.on === 'function',
    events: sessions && typeof ctx.on === 'function', readonlyStatus: sessions && typeof ctx.on === 'function',
    // A registered host hook does not prove the physical approval relay ready.
    approval: 'independent-relay',
  })
}
module.exports = { capabilities, serviceView }
