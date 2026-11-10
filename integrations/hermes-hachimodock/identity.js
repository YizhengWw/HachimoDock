'use strict'

// Device identity for state reports (issue #28, M1): session id + Hermes
// source + a hostname prefix, so the deskpet never mistakes this bridge for
// an installed agent (the #15 OpenClaw misidentification) and can tell
// concurrent sessions / sources apart.
const os = require('node:os');

const sanitize = (value) => String(value || '')
  .toLowerCase()
  .replace(/[^a-z0-9._-]/g, '-')
  .replace(/-+/g, '-')
  .replace(/^-+|-+$/g, '');

/**
 * @param {{sessionId?:string, source?:string}} [input]
 * @returns {{host:string, sessionId:string, source:string}}
 */
function deviceIdentity({ sessionId, source } = {}) {
  return {
    host: `hermes-${sanitize(os.hostname()) || 'unknown'}`,
    sessionId: typeof sessionId === 'string' ? sessionId : '',
    source: typeof source === 'string' && source ? source : 'unknown',
  };
}

module.exports = { deviceIdentity };
