#!/usr/bin/env node
'use strict'
// Explicit local pairing control; never auto-pair from browser/health/session data.
const { readDescriptor } = require('./capability')
async function main() {
  const [origin, sessionId] = process.argv.slice(2)
  if (!origin || !sessionId || process.argv.length !== 4) throw new Error('usage: node pair.js EXACT_ORIGIN EXACT_SESSION_ID')
  const d = readDescriptor()
  const r = await fetch(d.baseUrl + '/pair/ticket', { method: 'POST', redirect: 'error', signal: AbortSignal.timeout(3000), headers: { Authorization: `Bearer ${d.tokens.voice}`, 'Content-Type': 'application/json' }, body: JSON.stringify({ origin, sessionId }) })
  const result = await r.json()
  if (!r.ok || result.ok !== true) throw new Error('pairing ticket unavailable')
  // A user-requested one-time ticket only; never print native capability tokens.
  console.log(`Paste this one-use code into the selected session's HachimoDock dock within 120s:\n${result.code}`)
}
main().catch(() => { console.error('DSH pairing unavailable; verify exact Origin/session and private bootstrap permissions'); process.exitCode = 1 })
