'use strict'

// Private local bootstrap. No credential is accepted from URL/query parameters.
const fs = require('node:fs')
const path = require('node:path')
const os = require('node:os')
const { randomBytes, timingSafeEqual } = require('node:crypto')
const secret = () => randomBytes(32).toString('hex')
const descriptorPath = () => process.env.HACHIMODOCK_DSH_CAPABILITY_FILE || path.join(os.homedir(), '.claw-pet', 'dsh-bridge', 'capability.json')
function equal(a, b) {
  return typeof a === 'string' && typeof b === 'string' && Buffer.byteLength(a) === Buffer.byteLength(b) && timingSafeEqual(Buffer.from(a), Buffer.from(b))
}
function privateStat(file, directory = false) {
  if (process.platform === 'win32') throw new Error('DSH owner-only ACL verification unavailable')
  const st = fs.lstatSync(file)
  if (st.isSymbolicLink() || (directory ? !st.isDirectory() : !st.isFile()) ||
      (process.getuid && st.uid !== process.getuid()) || (st.mode & 0o077)) throw new Error('unsafe capability permissions')
  return st
}
function readDescriptor(file = descriptorPath()) {
  privateStat(path.dirname(file), true)
  const fd = fs.openSync(file, fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW || 0))
  try {
    const before = privateStat(file)
    const st = fs.fstatSync(fd)
    if (st.ino !== before.ino || st.dev !== before.dev || st.size > 4096) throw new Error('unsafe capability descriptor')
    const d = JSON.parse(fs.readFileSync(fd, 'utf8'))
    const u = new URL(d.baseUrl)
    if (d.version !== 2 || !/^[a-f0-9]{64}$/.test(d.epoch) || u.protocol !== 'http:' || u.hostname !== '127.0.0.1' || !u.port || u.username || u.password || u.pathname !== '/' || u.search || u.hash ||
        !['relay', 'bus', 'voice'].every(k => /^[a-f0-9]{64}$/.test(d.tokens?.[k]))) throw new Error('invalid capability descriptor')
    return d
  } finally { fs.closeSync(fd) }
}
function writeDescriptor(d, file = descriptorPath()) {
  const dir = path.dirname(file)
  fs.mkdirSync(dir, { recursive: true, mode: 0o700 })
  privateStat(dir, true)
  if (fs.existsSync(file)) privateStat(file)
  const tmp = path.join(dir, `.capability-${secret()}.tmp`)
  const fd = fs.openSync(tmp, 'wx', 0o600)
  try { fs.writeFileSync(fd, JSON.stringify(d)); fs.fsyncSync(fd) } finally { fs.closeSync(fd) }
  try { fs.renameSync(tmp, file) } finally { if (fs.existsSync(tmp)) fs.unlinkSync(tmp) }
}
module.exports = { secret, equal, descriptorPath, readDescriptor, writeDescriptor }
