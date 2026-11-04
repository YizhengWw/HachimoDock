'use strict'

/**
 * Cordis plugin entry for the HachimoDock <-> DeepSeek Harness bridge.
 *
 * The package is plain CommonJS with no npm dependencies, so DSH's Loader can
 * import it by package name from a profile (`import()` of a CommonJS package
 * yields `module.exports` as the default export, which the Loader's
 * `unwrapExports` recognizes).
 *
 * Mounting it as a Host plugin starts a loopback HTTP/SSE endpoint used by the
 * HachimoDock PC bridge, registers the `approval/request` answerer, and
 * forwards compact session/turn/tool/title events. See README.md for the wire
 * contract and install instructions.
 *
 * @module @hachimodock/dsh-bridge
 */

const { createBridge, AGENT_ID, PLUGIN_NAME } = require('./dsh-bridge')
const { capabilities, serviceView } = require('./host-contract')

function apply(ctx, config) {
  const bridge = createBridge(ctx, config)
  // get(name) is the public optional-service lookup. Read each independent
  // service at the point of use, never couple agents to controller activation.
  bridge.ctx = serviceView(ctx)
  bridge.server.capabilities = () => capabilities(bridge.ctx)
  ctx.effect(async () => {
    await bridge.start()
    return () => bridge.stop()
  }, 'hachimodock: http bridge')
}

/** @type {string} Cordis plugin display name. */
const name = PLUGIN_NAME
/** @type {string[]} Cordis services the plugin waits for before mounting. */
const inject = ['sessions']

module.exports = {
  name,
  inject,
  apply,
  AGENT_ID,
}
