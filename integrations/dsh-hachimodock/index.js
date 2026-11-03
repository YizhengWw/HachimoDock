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

const { apply, AGENT_ID, PLUGIN_NAME, INJECT } = require('./dsh-bridge')

/** @type {string} Cordis plugin display name. */
const name = PLUGIN_NAME
/** @type {string[]} Cordis services the plugin waits for before mounting. */
const inject = INJECT

module.exports = {
  name,
  inject,
  apply,
  AGENT_ID,
}
