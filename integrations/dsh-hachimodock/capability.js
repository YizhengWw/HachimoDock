'use strict'

// Private local bootstrap. No credential is accepted from URL/query parameters.
const fs = require('node:fs')
const path = require('node:path')
const os = require('node:os')
const { randomBytes, timingSafeEqual } = require('node:crypto')
const { windowsAcl, windowsPath } = require('./windows-acl')
const secret = () => randomBytes(32).toString('hex')
const descriptorPath = () => process.env.HACHIMODOCK_DSH_CAPABILITY_FILE || path.join(os.homedir(), '.claw-pet', 'dsh-bridge', 'capability.json')
function equal(a, b) {
  return typeof a === 'string' && typeof b === 'string' && Buffer.byteLength(a) === Buffer.byteLength(b) && timingSafeEqual(Buffer.from(a), Buffer.from(b))
}
// Dependency injection exercises the real writer ordering and reader guards on
// non-Windows hosts; it is not a Windows filesystem/PowerShell execution claim.
function createCapabilityStore(options = {}) {
  const io = options.fs || fs, win = (options.platform || process.platform) === 'win32'
  const paths = win ? path.win32 : path
  const acl = options.acl || windowsAcl, mint = options.secret || secret
  const uid = options.uid || (() => process.getuid?.())
  function privateStat(file, directory = false) {
    const st = io.lstatSync(file)
    if (st.isSymbolicLink() || (directory ? !st.isDirectory() : !st.isFile()) ||
        (!win && (uid() !== undefined && st.uid !== uid() || st.mode & 0o077))) throw new Error('unsafe capability permissions')
    return st
  }
  function existing(file) {
    try { return io.lstatSync(file) } catch (e) { if (e.code === 'ENOENT') return null; throw e }
  }
  function prepareDirectory(file = descriptorPath()) {
    if (win) { file = windowsPath(file); acl('prepare-directory', paths.dirname(file)) }
    else io.mkdirSync(paths.dirname(file), { recursive: true, mode: 0o700 })
    privateStat(paths.dirname(file), true)
    if (existing(file)) {
      privateStat(file)
      // A disclosed existing token is never read/reused. Repair only its ACL
      // after the directory is private, then replace with a newly minted epoch.
      if (win) acl('protect-file', file)
    }
  }
  function readDescriptor(file = descriptorPath()) {
    if (win) { file = windowsPath(file); acl('verify-file', file) }
    privateStat(paths.dirname(file), true)
    const fd = io.openSync(file, io.constants.O_RDONLY | (io.constants.O_NOFOLLOW || 0))
    try {
      const before = privateStat(file), st = io.fstatSync(fd)
      if (!st.isFile() || st.ino !== before.ino || st.dev !== before.dev || st.size > 4096 || !win && (uid() !== undefined && st.uid !== uid() || st.mode & 0o077)) throw new Error('unsafe capability descriptor')
      if (win) acl('verify-file', file)
      const d = JSON.parse(io.readFileSync(fd, 'utf8'))
      const u = new URL(d.baseUrl)
      if (d.version !== 2 || !/^[a-f0-9]{64}$/.test(d.epoch) || u.protocol !== 'http:' || u.hostname !== '127.0.0.1' || !u.port || u.username || u.password || u.pathname !== '/' || u.search || u.hash ||
          !['relay', 'bus', 'voice'].every(k => /^[a-f0-9]{64}$/.test(d.tokens?.[k]))) throw new Error('invalid capability descriptor')
      return d
    } finally { io.closeSync(fd) }
  }
  function writeDescriptor(d, file = descriptorPath()) {
    prepareDirectory(file)
    const dir = paths.dirname(file), tmp = paths.join(dir, `.capability-${mint()}.tmp`)
    let fd, created = false
    try {
      // Windows creates the empty temp with explicit owner + protected DACL,
      // rather than relying on the elevated token's default object owner.
      if (win) { acl('create-file', tmp); created = true }
      fd = io.openSync(tmp, win ? 'r+' : 'wx', 0o600)
      created = true
      if (win) {
        acl('verify-file', tmp)
        const st = privateStat(tmp), opened = io.fstatSync(fd)
        if (!opened.isFile() || st.ino !== opened.ino || st.dev !== opened.dev || opened.size !== 0) throw new Error('unsafe new capability file')
      }
      io.writeFileSync(fd, JSON.stringify(d)); io.fsyncSync(fd)
    } catch (e) {
      if (fd !== undefined) io.closeSync(fd)
      // CreateNew rejection must never delete a preexisting entry.
      if (created && existing(tmp)) io.unlinkSync(tmp)
      throw e
    }
    io.closeSync(fd)
    try {
      io.renameSync(tmp, file)
      if (win) acl('verify-file', file)
      else privateStat(file)
    } finally { if (existing(tmp)) io.unlinkSync(tmp) }
  }
  return { readDescriptor, writeDescriptor, prepareDirectory }
}
const store = createCapabilityStore()
module.exports = { secret, equal, descriptorPath, createCapabilityStore, ...store }
