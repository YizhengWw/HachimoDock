'use strict'

// Local configuration only. No credentials, capability descriptor, sessions or
// host requests are read here. All shipped bytes come from the positive list.
const fs = require('node:fs')
const path = require('node:path')
const os = require('node:os')
const net = require('node:net')
const { createHash, randomBytes } = require('node:crypto')
const { pathToFileURL } = require('node:url')
const yaml = require('./vendor/js-yaml.cjs')
const { windowsAcl } = require('./windows-acl')
const { profileMetadataAcl } = require('./profile-metadata-windows-acl')
const FILES = ['package.json', 'index.js', 'dsh-bridge.js', 'server.js', 'client.js', 'capability.js', 'windows-acl.js', 'windows-acl.ps1', 'host-contract.js', 'dsh-state-contract.js', 'profile-metadata-windows-acl.js', 'profile-metadata-windows-acl.ps1']
const BEGIN = '# BEGIN HACHIMODOCK MANAGED BRIDGE v1'
const END = '# END HACHIMODOCK MANAGED BRIDGE v1'
const hash = bytes => createHash('sha256').update(bytes).digest('hex')
const fail = code => { throw new Error(code) }
// !!js in an existing user patch stays inert: it is never evaluated or dumped.
const schema = yaml.FAILSAFE_SCHEMA.extend(new yaml.Type('tag:yaml.org,2002:js', { kind: 'scalar', construct: s => s }))

function check(p, directory, privateEntry = false) {
  const s = fs.lstatSync(p)
  if (s.isSymbolicLink() || (directory ? !s.isDirectory() : !s.isFile())) fail('unsafe-profile-entry')
  if (process.platform === 'win32') {
    const operation = directory ? 'verify-directory' : 'verify-file'
    if (privateEntry) windowsAcl(operation, p)
    else profileMetadataAcl(operation, p)
  }
  else if (s.uid !== process.getuid() || (s.mode & (privateEntry ? 0o077 : 0o022))) fail('unsafe-profile-permissions')
  return s
}
function read(p, max = 262144, privateEntry = false) {
  check(p, false, privateEntry)
  const fd = fs.openSync(p, fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW || 0))
  try {
    const s = fs.fstatSync(fd), before = fs.lstatSync(p)
    if (s.size > max || s.ino !== before.ino || s.dev !== before.dev) fail('profile-changed')
    return fs.readFileSync(fd)
  } finally { fs.closeSync(fd) }
}
function mkdir(p) {
  if (!fs.existsSync(p)) {
    if (process.platform === 'win32') windowsAcl('prepare-directory', p)
    else fs.mkdirSync(p, { mode: 0o700 })
  }
  check(p, true, true)
}
function exclusive(p, bytes) {
  if (process.platform === 'win32') windowsAcl('create-file', p)
  const fd = fs.openSync(p, process.platform === 'win32' ? 'r+' : 'wx', 0o600)
  try { fs.writeFileSync(fd, bytes); fs.fsyncSync(fd) } finally { fs.closeSync(fd) }
  check(p, false, true)
}
function profilePaths() {
  const home = path.resolve(os.homedir())
  check(home, true)
  // This is the default desktop profile contract; no scanning user credentials.
  const profile = path.join(home, '.dsh', 'profiles', 'desktop')
  let p = home
  for (const part of ['.dsh', 'profiles', 'desktop']) { p = path.join(p, part); check(p, true) }
  const manifest = JSON.parse(read(path.join(profile, 'package.json'), 65536))
  if (!Array.isArray(manifest.dsh?.profile?.bundles) || !manifest.dsh.profile.bundles.includes('@deepseek-ai/dsh-web-app')) fail('unsupported-desktop-profile-contract')
  return { profile, patch: path.join(profile, 'cordis.patch.yml'), managed: path.join(profile, '.hachimodock-managed') }
}
function validatePatch(bytes) {
  const raw = bytes.toString('utf8')
  if (Buffer.from(raw).compare(bytes) || raw.includes('\0')) fail('unsupported-profile-encoding')
  const value = yaml.load(raw, { schema })
  if (!Array.isArray(value) || value.some(v => !v || typeof v !== 'object' || Array.isArray(v))) fail('unsupported-profile-patch-contract')
  // Preserve block-sequence bytes. Flow arrays/document directives need an
  // explicit adapter, never a lossy YAML parse/dump of a user's patch.
  const lines = raw.split(/\r?\n/).filter(l => l.trim() && !l.trimStart().startsWith('#'))
  if (value.length === 0 && lines.length === 1 && /^[ \t]*\[\][ \t]*(?:#.*)?$/.test(lines[0])) return raw
  if (!lines.length || !lines[0].startsWith('- ') || lines.some(l => !/^\s/.test(l) && !l.startsWith('- '))) fail('unsupported-profile-patch-layout')
  return raw
}
async function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer()
    server.once('error', () => reject(new Error('loopback-port-unavailable')))
    server.listen({ host: '127.0.0.1', port: 0, exclusive: true }, () => {
      const port = server.address().port
      server.close(error => error ? reject(error) : resolve(port))
    })
  })
}
async function install() {
  const { profile, patch, managed } = profilePaths()
  const original = read(patch), raw = validatePatch(original)
  mkdir(managed)
  let existing = null
  const start = raw.indexOf(BEGIN), end = raw.indexOf(END)
  if (start >= 0 || end >= 0) {
    if (start < 0 || end < start || raw.indexOf(BEGIN, start + 1) >= 0 || raw.indexOf(END, end + 1) >= 0
        || start > 0 && raw[start - 1] !== '\n') fail('managed-block-conflict')
    if (raw.slice(end + END.length).length && !/^\r?\n/.test(raw.slice(end + END.length))) fail('managed-block-conflict')
    const blockEnd = end + END.length + (raw.slice(end + END.length).startsWith('\r\n') ? 2 : raw[end + END.length] === '\n' ? 1 : 0)
    const block = raw.slice(start, blockEnd)
    if (/hachimodock-bridge|HACHIMODOCK MANAGED BRIDGE/.test(raw.slice(0, start) + raw.slice(blockEnd))) fail('foreign-bridge-entry')
    const receipts = fs.readdirSync(managed).filter(n => /^install-[0-9]+-[a-f0-9]{12}\.json$/.test(n)).sort().reverse()
    for (const name of receipts) {
      const r = JSON.parse(read(path.join(managed, name), 16384, true))
      if (r.version === 1 && r.blockHash === hash(Buffer.from(block)) && /^[a-f0-9]{64}$/.test(r.package)) {
        const u = new URL(r.baseUrl)
        if (u.protocol !== 'http:' || u.hostname !== '127.0.0.1' || !u.port || u.origin !== r.baseUrl) fail('invalid-managed-connection')
        const installed = path.join(managed, r.package)
        check(installed, true, true)
        if (!r.files || ['package.json', 'index.js', 'server.js', 'client.js', 'dsh-bridge.js', 'capability.js'].some(n => !r.files[n]) || Object.keys(r.files).some(n => !FILES.includes(n) || !/^[a-f0-9]{64}$/.test(r.files[n] || ''))) fail('invalid-managed-receipt')
        for (const name of Object.keys(r.files)) {
          const data = read(path.join(installed, name), 300000, true)
          if (hash(data) !== r.files[name]) fail('managed-package-changed')
          if (name.endsWith('.js')) for (const match of data.toString('utf8').matchAll(/require\(['"](\.[^'"]+)['"]\)/g)) {
            const relative = path.posix.normalize(path.posix.join(path.posix.dirname(name), match[1]))
            const dependency = /\.(?:js|json|cjs)$/.test(relative) ? relative : relative + '.js'
            if (!r.files[dependency]) fail('incomplete-managed-package')
          }
        }
        existing = { receipt: r, start, blockEnd, block }; break
      }
    }
    if (!existing) fail('unowned-managed-entry')
  } else if (/hachimodock-bridge/.test(raw)) fail('foreign-bridge-entry')
  // This lock belongs only to our installer, not the host's undocumented lock
  // protocol. Refuse an active host lock; never write/steal/remove its lock.
  if (fs.existsSync(path.join(profile, 'lock'))) fail('host-profile-busy')
  const lock = path.join(managed, 'install.lock')
  let locked = false
  try {
    exclusive(lock, String(process.pid)); locked = true
    if (!read(patch).equals(original)) fail('profile-changed')
    const bytes = new Map()
    for (const name of FILES) {
      const source = path.join(__dirname, name)
      const st = fs.lstatSync(source)
      if (!st.isFile() || st.isSymbolicLink() || st.size > 300000) fail('invalid-bundled-plugin')
      bytes.set(name, fs.readFileSync(source))
    }
    const sourceHash = hash(Buffer.concat([...bytes].flatMap(([name, data]) => [Buffer.from(name + '\0'), data])))
    // Loader name-only updates can retain the old module/fiber. A changed
    // immutable public package gets a distinct entry id and a fresh port so
    // profile HMR removes the old entry and imports the new module, without
    // racing asynchronous disposal of its previous loopback endpoint.
    const port = existing?.receipt.sourceHash === sourceHash
      ? Number(new URL(existing.receipt.baseUrl).port) : await freePort()
    const base = `http://127.0.0.1:${port}`
    const marker = "const base = /* HACHIMODOCK_MANAGED_BASE */ 'http://127.0.0.1:17892'"
    const client = bytes.get('client.js').toString('utf8')
    const endpointParts = client.split(marker)
    if (endpointParts.length > 2) fail('unsupported-client-connection-contract')
    // The status-only client has no loopback endpoint or pairing UI. Preserve
    // its bundled bytes; only the legacy transport client needs port injection.
    if (endpointParts.length === 2) bytes.set('client.js', Buffer.from(client.replace(marker, `const base = /* HACHIMODOCK_MANAGED_BASE */ ${JSON.stringify(base)}`)))
    const digest = hash(Buffer.concat([...bytes].flatMap(([name, data]) => [Buffer.from(name + '\0'), data])))
    const installed = path.join(managed, digest)
    mkdir(installed)
    for (const [name, data] of bytes) {
      const dest = path.join(installed, name)
      if (fs.existsSync(dest)) { if (!read(dest, 300000, true).equals(data)) fail('managed-package-changed') }
      else exclusive(dest, data)
    }
    const nl = raw.includes('\r\n') ? '\r\n' : '\n'
    const entry = [BEGIN, '- insert:', `    - id: hachimodock-bridge-${digest.slice(0, 24)}`, `      name: ${JSON.stringify(pathToFileURL(path.join(installed, 'index.js')).href)}`, '      config:', `        port: ${port}`, '        approvalTimeoutMs: 30000', '        approvalFallback: delegate', '        allowedOrigins: ["dsh-app://app"]', END, ''].join(nl)
    if (existing && existing.block === entry && existing.receipt.package === digest) {
      return { ok: true, configured: true, changed: false, reloadPending: true }
    }
    // The host's official fresh-profile template is []. Remove only those
    // two syntax bytes, preserving all its comments/whitespace in the backup
    // and resulting block document. Nonempty flow sequences remain unsupported.
    const prefix = yaml.load(raw, { schema }).length === 0
      ? raw.replace(/^([ \t]*)\[\]([ \t]*(?:#[^\r\n]*)?)(\r?)$/m, '$1$2$3') : raw
    const next = Buffer.from(existing
      ? raw.slice(0, existing.start) + entry + raw.slice(existing.blockEnd)
      : prefix + (prefix.endsWith('\n') ? '' : nl) + entry)
    validatePatch(next)
    const id = `${Date.now()}-${randomBytes(6).toString('hex')}`
    const backup = path.join(managed, `profile-${id}.backup`)
    exclusive(backup, original)
    const temp = path.join(managed, `profile-${id}.tmp`)
    // Windows strict ACL helpers require their directory be private too. Fail
    // closed for profiles that do not meet that contract; don't change theirs.
    exclusive(temp, next)
    if (!read(patch).equals(original)) { fs.unlinkSync(temp); fail('profile-changed') }
    const receipt = { version: 1, backup: path.basename(backup), before: hash(original), after: hash(next), package: digest, baseUrl: base, origin: 'dsh-app://app', profileContract: 'desktop-loader-patch-v1', sourceHash, blockHash: hash(Buffer.from(entry)), files: Object.fromEntries([...bytes].map(([name, data]) => [name, hash(data)])) }
    exclusive(path.join(managed, `install-${id}.json`), Buffer.from(JSON.stringify(receipt)))
    fs.renameSync(temp, patch)
    return { ok: true, configured: true, changed: true, reloadPending: true, contract: receipt.profileContract }
  } finally { if (locked) fs.unlinkSync(lock) }
}
function rollback() {
  const { profile, patch, managed } = profilePaths()
  check(managed, true, true)
  const names = fs.readdirSync(managed).filter(n => /^install-[0-9]+-[a-f0-9]{12}\.json$/.test(n)).sort().reverse()
  if (!names.length) fail('no-managed-install')
  const r = JSON.parse(read(path.join(managed, names[0]), 16384, true))
  if (!/^profile-[0-9]+-[a-f0-9]{12}\.backup$/.test(r.backup)) fail('invalid-managed-receipt')
  const original = read(path.join(managed, r.backup), 262144, true), current = read(patch)
  if (hash(current) !== r.after || hash(original) !== r.before) fail('profile-changed-rollback-refused')
  if (fs.existsSync(path.join(profile, 'lock'))) fail('host-profile-busy')
  const lock = path.join(managed, 'install.lock')
  exclusive(lock, String(process.pid))
  try {
    const temp = path.join(managed, 'rollback-' + randomBytes(8).toString('hex'))
    exclusive(temp, original)
    if (!read(patch).equals(current)) { fs.unlinkSync(temp); fail('profile-changed') }
    fs.renameSync(temp, patch)
    return { ok: true, configured: false, reloadPending: true, rolledBack: true }
  } finally { fs.unlinkSync(lock) }
}
if (require.main === module) {
  if (process.argv.length !== 3 || !['--install', '--rollback'].includes(process.argv[2])) { process.stdout.write(JSON.stringify({ ok: false, code: 'invalid-operation' })); process.exitCode = 1 }
  else Promise.resolve().then(() => process.argv[2] === '--install' ? install() : rollback()).then(r => process.stdout.write(JSON.stringify(r))).catch(error => {
    const safe = /^[a-z][a-z-]{2,80}$/.test(error.message) ? error.message : 'local-install-unavailable'
    process.stdout.write(JSON.stringify({ ok: false, code: safe })); process.exitCode = 1
  })
}
module.exports = { install, rollback }
