'use strict'
const fs = require('node:fs')
const path = require('node:path')
const { spawnSync } = require('node:child_process')
const SYSTEM = 'S-1-5-18', FULL = 2032127
const OPERATIONS = new Set(['prepare-directory', 'create-file', 'protect-file', 'verify-directory', 'verify-file'])
function windowsPath(value) {
  if (typeof value !== 'string' || value.length > 2048 || !/^[A-Za-z]:\\/.test(value) || /[\x00-\x1f]/.test(value) || value.slice(2).includes(':') || value.includes('/')) throw new Error('invalid Windows capability path')
  const parts = value.slice(3).split('\\')
  if (parts.some(p => !p || p === '.' || p === '..' || /[. ]$/.test(p))) throw new Error('invalid Windows capability path')
  return path.win32.normalize(value)
}
function validateAclReceipt(receipt, operation, target) {
  if (!receipt || receipt.version !== 1 || receipt.operation !== operation || !/^S-1-(?:\d+-)+\d+$/.test(receipt.currentSid) || receipt.currentSid === SYSTEM || receipt.currentSid.length > 184) throw new Error('invalid Windows ACL receipt')
  const check = (entry, expectedPath, directory) => {
    if (!entry || typeof entry.path !== 'string' || entry.path.toLowerCase() !== expectedPath.toLowerCase() || entry.directory !== directory || entry.reparse !== false || entry.rawAceCount !== 2 || entry.owner !== receipt.currentSid || entry.protected !== true || !Array.isArray(entry.rules) || entry.rules.length !== 2) throw new Error('unsafe Windows capability ACL')
    const sids = new Set()
    for (const r of entry.rules) {
      if (![receipt.currentSid, SYSTEM].includes(r.sid) || sids.has(r.sid) || r.type !== 0 || r.inherited !== false || r.rights !== FULL || r.inheritance !== (directory ? 3 : 0) || r.propagation !== 0) throw new Error('unsafe Windows capability ACE')
      sids.add(r.sid)
    }
    if (!sids.has(receipt.currentSid) || !sids.has(SYSTEM)) throw new Error('missing Windows capability ACE')
  }
  target = windowsPath(target)
  const file = operation.endsWith('file')
  check(receipt.directory, file ? path.win32.dirname(target) : target, true)
  if (file) check(receipt.item, target, false)
  else if (receipt.item !== null) throw new Error('unexpected Windows capability item')
  return receipt
}
function windowsAcl(operation, target, options = {}) {
  if (!OPERATIONS.has(operation)) throw new Error('invalid Windows ACL operation')
  target = windowsPath(target)
  const env = options.env || process.env
  const systemRoot = windowsPath(env.SystemRoot || env.SYSTEMROOT || env.WINDIR)
  const executable = path.win32.join(systemRoot, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe')
  const script = fs.readFileSync(path.join(__dirname, 'windows-acl.ps1'), 'utf8')
  const result = (options.spawn || spawnSync)(executable, ['-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')], {
    windowsHide: true, encoding: 'utf8', timeout: 5000, maxBuffer: 32768,
    env: { SystemRoot: systemRoot, WINDIR: systemRoot, PSModulePath: path.win32.join(systemRoot, 'System32', 'WindowsPowerShell', 'v1.0', 'Modules'), HACHIMODOCK_DSH_ACL_OPERATION: operation, HACHIMODOCK_DSH_ACL_PATH: target },
    stdio: ['ignore', 'pipe', 'pipe'],
  })
  if (result.error || result.signal || result.status !== 0 || result.stderr?.trim() || typeof result.stdout !== 'string' || Buffer.byteLength(result.stdout) > 32768) throw new Error('Windows capability ACL verification failed')
  let receipt
  try { receipt = JSON.parse(result.stdout.replace(/^\uFEFF/, '').trim()) } catch { throw new Error('invalid Windows ACL receipt') }
  return validateAclReceipt(receipt, operation, target)
}
module.exports = { windowsPath, validateAclReceipt, windowsAcl }
