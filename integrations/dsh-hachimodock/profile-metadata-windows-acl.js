'use strict'

// Non-secret profile metadata only. Secret capability paths use windows-acl.js.
const fs = require('node:fs')
const path = require('node:path')
const { createHash, randomBytes } = require('node:crypto')
const { spawnSync } = require('node:child_process')
const SCHEMA = 'dsh-profile-metadata-acl-v1'
const SYSTEM = 'S-1-5-18'
const ADMINS = 'S-1-5-32-544'
const READ_ONLY = 0xa01200a9 // GenericRead/Execute, Read/Execute, ReadControl, Synchronize.
const OPERATIONS = new Set(['verify-directory', 'verify-file'])
const reject = () => { throw new Error('Windows profile metadata ACL verification unavailable or rejected') }

function profileWindowsPath(value) {
  if (typeof value !== 'string' || value.length < 3 || value.length > 2048 || !/^[A-Za-z]:\\/.test(value)
      || /[\x00-\x1f\x7f]/.test(value) || value.includes('/') || value.slice(2).includes(':')) reject()
  const parts = value.slice(3).split('\\')
  if (value.length > 3 && parts.some(part => !part || part === '.' || part === '..' || /[. ]$/.test(part)
      || /[<>"|?*]/.test(part) || /^(?:CON|PRN|AUX|NUL|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3]|CONIN\$|CONOUT\$)(?:\.|$)/i.test(part))) reject()
  if (path.win32.normalize(value) !== value || Buffer.from(value, 'utf8').toString('utf8') !== value) reject()
  return value
}
function exactKeys(value, keys) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || Object.getPrototypeOf(value) !== Object.prototype) reject()
  const actual = Object.keys(value).sort()
  if (actual.length !== keys.length || actual.some((key, i) => key !== [...keys].sort()[i])) reject()
}
function sid(value) {
  return typeof value === 'string' && value.length <= 184 && /^S-1-(?:\d+-)+\d+$/.test(value)
}
function binding(operation, target, nonce) {
  return createHash('sha256').update(operation + '\0' + target + '\0' + nonce, 'utf8').digest('hex')
}
function validateProfileMetadataReceipt(receipt, operation, target, nonce) {
  if (!OPERATIONS.has(operation)) reject()
  target = profileWindowsPath(target)
  if (typeof nonce !== 'string' || !/^[a-f0-9]{32}$/.test(nonce)) reject()
  exactKeys(receipt, ['schema', 'operation', 'binding', 'currentSid', 'ancestorCount', 'ancestorsNoReparse', 'item'])
  if (receipt.schema !== SCHEMA || receipt.operation !== operation || receipt.binding !== binding(operation, target, nonce)
      || !sid(receipt.currentSid) || [SYSTEM, ADMINS].includes(receipt.currentSid)
      || !Number.isInteger(receipt.ancestorCount) || receipt.ancestorCount < 1 || receipt.ancestorCount > 64
      || receipt.ancestorsNoReparse !== true) reject()
  const item = receipt.item
  exactKeys(item, ['directory', 'reparse', 'ownerSid', 'daclPresent', 'protected', 'aceCount', 'aces'])
  if (item.directory !== (operation === 'verify-directory') || item.reparse !== false || item.ownerSid !== receipt.currentSid
      || item.daclPresent !== true || typeof item.protected !== 'boolean' || !Number.isInteger(item.aceCount)
      || item.aceCount < 0 || item.aceCount > 128 || !Array.isArray(item.aces) || item.aces.length !== item.aceCount) reject()
  for (const ace of item.aces) {
    exactKeys(ace, ['sid', 'type', 'mask', 'flags'])
    if (!sid(ace.sid) || ![0, 1].includes(ace.type) || !Number.isInteger(ace.mask) || ace.mask < 0 || ace.mask > 0xffffffff
        || !Number.isInteger(ace.flags) || ace.flags < 0 || ace.flags > 31) reject()
    // Deny ACEs grant nothing. Reject foreign Allow writes even if another ACE denies them.
    // InheritOnly writes are rejected too: they could grant writes to future profile children.
    if (ace.type === 0 && ![receipt.currentSid, SYSTEM, ADMINS].includes(ace.sid)
        && ((ace.mask & ~READ_ONLY) >>> 0) !== 0) reject()
  }
  return receipt
}
function profileMetadataAcl(operation, target, options = {}) {
  if ((options.platform || process.platform) !== 'win32' || !OPERATIONS.has(operation)) reject()
  target = profileWindowsPath(target)
  const inputEnv = options.env || process.env
  const systemRoot = profileWindowsPath(inputEnv.SystemRoot || inputEnv.SYSTEMROOT || inputEnv.WINDIR)
  const executable = path.win32.join(systemRoot, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe')
  let script
  try { script = fs.readFileSync(path.join(__dirname, 'profile-metadata-windows-acl.ps1'), 'utf8') } catch { reject() }
  const nonce = randomBytes(16).toString('hex')
  let result
  try {
    result = (options.spawn || spawnSync)(executable,
      ['-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')], {
        windowsHide: true, encoding: 'utf8', timeout: 5000, maxBuffer: 65536,
        env: { SystemRoot: systemRoot, WINDIR: systemRoot,
          PSModulePath: path.win32.join(systemRoot, 'System32', 'WindowsPowerShell', 'v1.0', 'Modules'),
          HACHIMODOCK_PROFILE_METADATA_OPERATION: operation, HACHIMODOCK_PROFILE_METADATA_PATH: target,
          HACHIMODOCK_PROFILE_METADATA_NONCE: nonce },
        stdio: ['ignore', 'pipe', 'pipe'],
      })
  } catch { reject() }
  if (!result || result.error || result.signal || result.status !== 0 || (result.stderr && result.stderr.trim())
      || typeof result.stdout !== 'string' || Buffer.byteLength(result.stdout) > 65536) reject()
  let receipt
  try { receipt = JSON.parse(result.stdout.replace(/^\uFEFF/, '').trim()) } catch { reject() }
  return validateProfileMetadataReceipt(receipt, operation, target, nonce)
}
module.exports = { profileMetadataAcl, profileWindowsPath, validateProfileMetadataReceipt }
