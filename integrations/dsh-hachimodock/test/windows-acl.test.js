'use strict'
const test = require('node:test'), assert = require('node:assert/strict')
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm')
const { windowsAcl, windowsPath, validateAclReceipt } = require('../windows-acl')
const { createCapabilityStore } = require('../capability')
const SID = 'S-1-5-21-1-2-3-1001', SYSTEM = 'S-1-5-18', file = 'C:\\private\\capability.json'
function entry(name, directory) { return { path: name, directory, owner: SID, protected: true, reparse: false, rawAceCount: 2, rules: [SID, SYSTEM].map(sid => ({ sid, type: 0, inherited: false, rights: 2032127, inheritance: directory ? 3 : 0, propagation: 0 })) } }
function receipt(operation = 'verify-file', target = file) { const isFile = operation.endsWith('file'); return { version: 1, operation, currentSid: SID, directory: entry(isFile ? path.win32.dirname(target) : target, true), item: isFile ? entry(target, false) : null } }
function clone(x) { return JSON.parse(JSON.stringify(x)) }
test('Windows production receipt validator requires protected current SID + SYSTEM exclusively', () => {
  const good = receipt(); assert.equal(validateAclReceipt(good, 'verify-file', file), good)
  for (const key of ['directory', 'item']) {
    for (const [field, value] of [['owner', SYSTEM], ['protected', false], ['rawAceCount', 3], ['reparse', true], ['directory', key !== 'directory']]) {
      const bad = clone(good); bad[key][field] = value; assert.throws(() => validateAclReceipt(bad, 'verify-file', file))
    }
    for (const [field, value] of [['sid', 'S-1-1-0'], ['inherited', true], ['type', 1], ['rights', 1179785], ['inheritance', 7], ['propagation', 1]]) {
      const bad = clone(good); bad[key].rules[1][field] = value; assert.throws(() => validateAclReceipt(bad, 'verify-file', file))
    }
    const duplicate = clone(good); duplicate[key].rules[1].sid = SID; assert.throws(() => validateAclReceipt(duplicate, 'verify-file', file))
    const extra = clone(good); extra[key].rules.push({ ...extra[key].rules[1], sid: 'S-1-5-32-544' }); assert.throws(() => validateAclReceipt(extra, 'verify-file', file))
  }
  for (const field of ['currentSid', 'operation', 'version']) { const bad = clone(good); bad[field] = null; assert.throws(() => validateAclReceipt(bad, 'verify-file', file)) }
})
test('Windows path policy handles Unicode/spaces and rejects UNC, devices, ADS and ambiguous paths', () => {
  assert.equal(windowsPath('D:\\用户 Name\\dsh-bridge\\capability.json'), 'D:\\用户 Name\\dsh-bridge\\capability.json')
  for (const p of ['relative.json', '\\\\server\\share\\capability.json', '\\\\?\\C:\\private\\capability.json', 'C:\\private\\..\\capability.json', 'C:\\private\\capability.json:stream', 'C:\\private \\capability.json', 'C:\\private.\\capability.json', 'C:/private/capability.json', 'C:\\private\\\nfile', 'C:\\' + 'x'.repeat(2048)]) assert.throws(() => windowsPath(p), p)
})
test('8.3 caller spelling stays exact; an expanded receipt is never accepted as a different path', () => {
  const short = 'C:\\Users\\RUNNER~1\\AppData\\Local\\Temp\\private\\capability.json'
  assert.equal(windowsPath(short), short)
  const good = receipt('verify-file', short)
  assert.equal(validateAclReceipt(good, 'verify-file', short), good)
  const expanded = clone(good)
  expanded.directory.path = expanded.directory.path.replace('RUNNER~1', 'runneradmin')
  expanded.item.path = expanded.item.path.replace('RUNNER~1', 'runneradmin')
  assert.throws(() => validateAclReceipt(expanded, 'verify-file', short))
})
test('PowerShell invocation is fixed, bounded, NoProfile/NonInteractive; only path/operation enter child env', () => {
  let called = 0
  const result = windowsAcl('verify-file', file, { env: { SystemRoot: 'C:\\Windows', API_TOKEN: 'do-not-inherit' }, spawn(exe, args, opts) {
    called++; assert.equal(exe, 'C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe')
    assert.deepEqual(args.slice(0, -1), ['-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand'])
    const script = Buffer.from(args.at(-1), 'base64').toString('utf16le')
    assert.equal(script, fs.readFileSync(path.join(__dirname, '../windows-acl.ps1'), 'utf8'))
    assert.ok(script.includes('GetCurrent().User')); assert.ok(script.includes('Get-Acl -LiteralPath')); assert.ok(script.includes('SetAccessRuleProtection($true, $false)'))
    assert.equal(opts.timeout, 5000); assert.equal(opts.maxBuffer, 32768); assert.equal(opts.env.API_TOKEN, undefined)
    assert.equal(opts.env.HACHIMODOCK_DSH_ACL_PATH, file); assert.equal(args.join(' ').includes(file), false)
    assert.equal(script.includes('ReadAllText'), false); assert.equal(script.includes('Get-Content'), false)
    return { status: 0, stdout: JSON.stringify(receipt()), stderr: '' }
  } })
  assert.equal(called, 1); assert.equal(result.currentSid, SID)
})
test('PowerShell failure, timeout, bad/oversized output and insecure receipt fail closed', () => {
  for (const result of [{ error: Error('timeout') }, { status: 1, stdout: '{}', stderr: '' }, { status: 0, signal: 'SIGTERM', stdout: '{}', stderr: '' }, { status: 0, stdout: '{}', stderr: 'warning' }, { status: 0, stdout: 'not-json', stderr: '' }, { status: 0, stdout: 'x'.repeat(32769), stderr: '' }, { status: 0, stdout: JSON.stringify({ ...receipt(), currentSid: SYSTEM }), stderr: '' }]) assert.throws(() => windowsAcl('verify-file', file, { env: { SystemRoot: 'C:\\Windows' }, spawn: () => result }))
})
function fakeWriter({ failAcl, failTempAcl = false, foreignOld = false, link = false, tempCollision = false } = {}) {
  const log = [], entries = new Map([[path.win32.dirname(file), { directory: true, data: '' }], [file, { directory: false, data: 'OLD-DISCLOSED-TOKEN', insecure: true, link }]])
  if (tempCollision) entries.set('C:\\private\\.capability-new-tmp.tmp', { directory: false, data: 'EXISTING-NOT-OURS', protected: true })
  let fd = 0; const handles = new Map()
  const stat = p => {
    const e = entries.get(p); if (!e) throw Object.assign(Error('missing'), { code: 'ENOENT' })
    return { isSymbolicLink: () => !!e.link, isDirectory: () => e.directory, isFile: () => !e.directory, ino: 1, dev: 1, size: e.data.length, mode: 0o666, uid: 0 }
  }
  const io = { constants: { O_RDONLY: 0 }, lstatSync: stat, fstatSync: n => stat(handles.get(n)), openSync(p, flags) { log.push(['open', p, flags]); if (flags === 'wx') { assert.equal(entries.has(p), false); entries.set(p, { directory: false, data: '' }) } handles.set(++fd, p); return fd },
    readFileSync() { throw Error('old credential bytes must never be read during rotation') },
    writeFileSync(n, data) { log.push(['write', handles.get(n)]); assert.equal(entries.get(handles.get(n)).protected, true); entries.get(handles.get(n)).data = data },
    fsyncSync() {}, closeSync() {}, renameSync(a, b) { log.push(['rename', a, b]); entries.set(b, entries.get(a)); entries.delete(a) }, unlinkSync(p) { entries.delete(p) }, mkdirSync() { throw Error('PowerShell must prepare Windows directory') } }
  const store = createCapabilityStore({ platform: 'win32', fs: io, secret: () => { log.push(['mint-temp']); return 'new-tmp' }, acl(op, target) {
    log.push(['acl', op, target])
    if (op === failAcl || foreignOld && target === file || failTempAcl && op === 'create-file' && target !== file) throw Error('ACL rejected')
    if (op === 'create-file') {
      assert.equal(entries.get(path.win32.dirname(target)).protected, true)
      if (entries.has(target)) throw Error('already exists')
      entries.set(target, { directory: false, data: '', protected: true })
    }
    else if (op === 'prepare-directory') entries.get(target).protected = true
    else if (op === 'protect-file') { assert.equal(entries.get(path.win32.dirname(target)).protected, true); entries.get(target).protected = true; entries.get(target).insecure = false }
    else assert.equal(entries.get(target).protected, true)
    return receipt(op, target)
  } })
  return { log, entries, store }
}
test('actual writer secures directory, repairs old ACL without reading token, creates private empty temp before secret write and rotates', () => {
  const x = fakeWriter(); const d = { epoch: 'NEW-EPOCH', tokens: { bus: 'NEW-NOT-OLD' } }
  x.store.writeDescriptor(d, file)
  assert.deepEqual(x.log[0], ['acl', 'prepare-directory', 'C:\\private'])
  assert.ok(x.log.findIndex(e => e[1] === 'protect-file' && e[2] === file) < x.log.findIndex(e => e[0] === 'mint-temp'))
  const writeAt = x.log.findIndex(e => e[0] === 'write'), tempProtectAt = x.log.findIndex(e => e[1] === 'create-file' && e[2] !== file)
  assert.ok(tempProtectAt >= 0 && tempProtectAt < writeAt)
  assert.equal(x.entries.get(file).data, JSON.stringify(d)); assert.equal(x.entries.get(file).data.includes('OLD-DISCLOSED'), false)
  assert.equal(x.log.at(-1)[1], 'verify-file')
})
test('insecure directory/foreign owner/reparse or temp ACL failure never writes a new credential', () => {
  for (const options of [{ failAcl: 'prepare-directory' }, { foreignOld: true }, { link: true }, { failAcl: 'protect-file' }, { failTempAcl: true }]) {
    const x = fakeWriter(options); assert.throws(() => x.store.writeDescriptor({ tokens: 'NEW' }, file)); assert.equal(x.log.some(e => e[0] === 'write'), false)
  }
})
test('CreateNew collision preserves existing bytes and never writes credentials', () => {
  const x = fakeWriter({ tempCollision: true })
  assert.throws(() => x.store.writeDescriptor({ tokens: 'NEW' }, file))
  assert.equal(x.entries.get('C:\\private\\.capability-new-tmp.tmp').data, 'EXISTING-NOT-OURS')
  assert.equal(x.log.some(e => e[0] === 'write'), false)
})
test('Windows creation receives owner/protected ACL at creation and rejects foreign existing entries', () => {
  const script = fs.readFileSync(path.join(__dirname, '../windows-acl.ps1'), 'utf8')
  assert.ok(script.includes('[IO.Directory]::CreateDirectory($target, $newAcl)'))
  assert.ok(script.includes('[IO.FileMode]::CreateNew'))
  assert.ok(script.includes('[IO.FileOptions]::None, $newAcl)'))
  assert.ok(script.includes("if ($old.owner -ne $current.Value) { throw 'owner' }"))
  const good = receipt('create-file')
  assert.equal(validateAclReceipt(good, 'create-file', file), good)
  const bad = clone(good); bad.item.owner = 'S-1-5-32-544'
  assert.throws(() => validateAclReceipt(bad, 'create-file', file))
})
test('actual server secures auth directory before generating epoch/tokens, including failed prepare', async () => {
  const source = fs.readFileSync(path.join(__dirname, '../server.js'), 'utf8'), log = []
  const sandbox = { module: { exports: {} }, require(name) { if (name === './capability') return { equal: () => false, secret: () => { log.push('mint'); return 'secret' }, prepareDirectory() { log.push('prepare'); throw Error('ACL denied') }, writeDescriptor() { log.push('write') } }; return require(name) } }
  vm.runInNewContext(source, sandbox)
  const server = new sandbox.module.exports.BridgeServer({ bootstrapFile: file })
  assert.deepEqual(log, []); await assert.rejects(server.listen(), /ACL denied/); assert.deepEqual(log, ['prepare'])
})
test('distributed ACL implementations and native embedded reference have one byte-identical authority', () => {
  const base = path.resolve(__dirname, '../../..'), bus = path.join(base, 'pc/src-tauri/bridge/packages/agent-session-bus/src/adapters')
  for (const name of ['windows-acl.js', 'windows-acl.ps1']) assert.deepEqual(fs.readFileSync(path.join(bus, name)), fs.readFileSync(path.join(__dirname, '..', name)))
  const rust = fs.readFileSync(path.join(base, 'pc/src-tauri/src/dsh_windows_acl.rs'), 'utf8')
  assert.ok(rust.includes('include_str!("../../../integrations/dsh-hachimodock/windows-acl.ps1")'))
})
