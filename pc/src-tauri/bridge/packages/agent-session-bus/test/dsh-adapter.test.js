'use strict'
const test = require('node:test'), assert = require('node:assert/strict'), http = require('node:http')
const fs = require('node:fs'), path = require('node:path'), os = require('node:os')
const { DshAdapter, _internal } = require('../src/adapters/dsh')
const { windowsAcl, windowsPath } = require('../src/adapters/windows-acl')
const { spawnSync } = require('node:child_process')
const { createCapabilityStore } = require('../../../../../../integrations/dsh-hachimodock/capability')
const token = 'a'.repeat(64), epoch = 'b'.repeat(64)
// Capture only the production helper's fixed diagnostic schema. Nothing from
// raw PowerShell stderr (which may include paths) is forwarded to CI output.
function fixtureAcl(t, operation, target) {
  return windowsAcl(operation, target, { spawn(executable, args, options) {
    const result = spawnSync(executable, args, options)
    const match = /DSH_ACL_REJECTED:(\{[^\r\n]+\})/.exec(result.stderr || '')
    if (match) {
      try {
        const d = JSON.parse(match[1])
        const stages = ['input', 'reparse', 'attributes', 'get-acl', 'owner', 'access-rules', 'raw-dacl', 'validate', 'acl-build', 'existing-owner', 'set-acl', 'create-directory', 'create-file']
        const causes = ['other', 'owner', 'acl', 'reparse', 'kind', 'path', 'user', 'operation', 'local-fixed-volume-required']
        const exceptions = ['other', 'RuntimeException', 'MethodInvocationException', 'UnauthorizedAccessException', 'IOException', 'ArgumentException', 'CommandNotFoundException', 'PropertyNotFoundException', 'ParameterBindingException']
        if (stages.includes(d.stage) && causes.includes(d.cause) && exceptions.includes(d.exception) &&
            [null, true, false].includes(d.ownerMatches) && [null, true, false].includes(d.protected) &&
            (d.aceCount === null || Number.isInteger(d.aceCount) && d.aceCount >= 0 && d.aceCount <= 4096)) {
          t.diagnostic(JSON.stringify({ operation, stage: d.stage, cause: d.cause, exception: d.exception, ownerMatches: d.ownerMatches, protected: d.protected, aceCount: d.aceCount }))
        }
      } catch { /* Never print malformed/native stderr. */ }
    }
    return result
  } })
}
let defaultOwnerProbed = false
function probeDefaultWindowsOwner(t, container) {
  if (defaultOwnerProbed) return
  defaultOwnerProbed = true
  const file = path.join(container, 'empty-owner-probe.tmp')
  fs.writeFileSync(file, '', { flag: 'wx' })
  const root = windowsPath(process.env.SystemRoot || process.env.SYSTEMROOT || process.env.WINDIR)
  const script = String.raw`
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$WarningPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$current = [Security.Principal.WindowsIdentity]::GetCurrent().User
$rows = @()
foreach ($kind in @('directory', 'file')) {
    $row = @{ kind = $kind; stage = 'get-acl'; ownerMatches = $null; protected = $null; aceCount = $null; exception = 'none' }
    try {
        $name = [Environment]::GetEnvironmentVariable('DSH_FIXTURE_PROBE_DIRECTORY')
        if ($kind -eq 'file') { $name = [IO.Path]::Combine($name, 'empty-owner-probe.tmp') }
        $acl = Microsoft.PowerShell.Security\Get-Acl -LiteralPath $name
        $row.stage = 'owner'
        $row.ownerMatches = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -eq $current.Value
        $row.protected = [Bool] $acl.AreAccessRulesProtected
        $row.stage = 'raw-dacl'
        $raw = [Security.AccessControl.RawSecurityDescriptor]::new($acl.GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]::Access))
        $row.aceCount = $raw.DiscretionaryAcl.Count
        $row.stage = 'complete'
    } catch {
        $kindName = $_.Exception.GetType().Name
        $row.exception = if ($kindName -in @('RuntimeException', 'MethodInvocationException', 'UnauthorizedAccessException', 'IOException', 'ArgumentException', 'CommandNotFoundException', 'PropertyNotFoundException', 'ParameterBindingException')) { $kindName } else { 'other' }
    }
    $rows += $row
}
$rows | Microsoft.PowerShell.Utility\ConvertTo-Json -Compress
`
  const r = spawnSync(path.win32.join(root, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe'),
    ['-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')],
    { encoding: 'utf8', windowsHide: true, timeout: 5000, maxBuffer: 32768, stdio: ['ignore', 'pipe', 'pipe'],
      env: { SystemRoot: root, WINDIR: root, PSModulePath: path.win32.join(root, 'System32', 'WindowsPowerShell', 'v1.0', 'Modules'), DSH_FIXTURE_PROBE_DIRECTORY: container } })
  try {
    assert.equal(r.status, 0)
    const rows = JSON.parse(r.stdout.replace(/^\uFEFF/, '').trim())
    assert.equal(rows.length, 2)
    for (const d of rows) {
      assert.ok(['directory', 'file'].includes(d.kind) && ['get-acl', 'owner', 'raw-dacl', 'complete'].includes(d.stage))
      assert.ok(['none', 'other', 'RuntimeException', 'MethodInvocationException', 'UnauthorizedAccessException', 'IOException', 'ArgumentException', 'CommandNotFoundException', 'PropertyNotFoundException', 'ParameterBindingException'].includes(d.exception))
      assert.ok([null, true, false].includes(d.ownerMatches) && [null, true, false].includes(d.protected))
      assert.ok(d.aceCount === null || Number.isInteger(d.aceCount) && d.aceCount >= 0 && d.aceCount <= 4096)
      t.diagnostic(JSON.stringify({ probe: 'default-object-security', kind: d.kind, stage: d.stage, ownerMatches: d.ownerMatches, protected: d.protected, aceCount: d.aceCount, exception: d.exception }))
    }
  } catch { t.diagnostic('default-object-security probe unavailable; raw output withheld') }
}
async function fixture(t, handler) {
  const server = http.createServer((req, res) => {
    assert.equal(req.headers.authorization, `Bearer ${token}`)
    Promise.resolve(handler(req, res)).catch(e => res.destroy(e))
  })
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const container = fs.mkdtempSync(path.join(os.tmpdir(), 'dsh-bus-'))
  const dir = path.join(container, 'private')
  // Register cleanup before ACL setup: a failed secure setup must fail the
  // test without leaving a listening server behind.
  t.after(async () => { server.closeAllConnections(); await new Promise(r => server.close(r)); fs.rmSync(container, { recursive: true, force: true }) })
  const file = path.join(dir, 'capability.json'), baseUrl = `http://127.0.0.1:${server.address().port}`
  const descriptor = { version: 2, epoch, baseUrl, tokens: { bus: token } }
  if (process.platform === 'win32') {
    probeDefaultWindowsOwner(t, container)
    // Exercise the real production writer: secure creation, empty-file check,
    // secret write, atomic rename and final ACL verification, with no bypass.
    createCapabilityStore({ acl: (op, target) => fixtureAcl(t, op, target) }).writeDescriptor(descriptor, file)
  } else {
    fs.mkdirSync(dir, { mode: 0o700 })
    fs.writeFileSync(file, JSON.stringify(descriptor), { mode: 0o600, flag: 'wx' })
  }
  return { file, dir, baseUrl, adapter: new DshAdapter({ env: { HACHIMODOCK_DSH_CAPABILITY_FILE: file } }) }
}
function sse(res, frames) { res.writeHead(200, { 'Content-Type': 'text/event-stream' }); for (const [e, d] of frames) res.write(`event: ${e}\ndata: ${JSON.stringify(d)}\n\n`); res.end() }
async function collect(a, request) { const events = []; for await (const e of a.inject(request)) events.push(e); return events }

test('endpoint rejects arbitrary hosts, userinfo, redirects targets, paths and invalid overrides', () => {
  for (const u of ['https://127.0.0.1:17892', 'http://evil.test:17892', 'http://127.0.0.1:17892/path', 'http://user@127.0.0.1:17892', 'not a url']) assert.throws(() => _internal.resolveBaseUrl(u))
})
test('availability authenticates actual descriptor endpoint via sessions, never public health', async t => {
  const { adapter } = await fixture(t, (req, res) => { assert.equal(req.url, '/sessions'); res.end(JSON.stringify({ version: 2, sessions: [] })) })
  assert.deepEqual(await adapter.isAvailable(), { ready: true, reason: null, version: 2 })
})
test('network budgets start after private descriptor verification, including synchronous Windows ACL checks', async t => {
  let requests = 0, verified = false, validations = 0
  const { adapter } = await fixture(t, (req, res) => {
    requests++
    if (req.url === '/inject') sse(res, [['done', { sessionId: 'a' }]])
    else res.end(JSON.stringify({ version: 2, sessions: [] }))
  })
  const auth = adapter._auth.bind(adapter), timeout = AbortSignal.timeout
  const budgets = []
  t.mock.method(adapter, '_auth', () => {
    assert.equal(verified, false)
    const headers = auth() // Real descriptor and, on Windows, real PowerShell ACL validation.
    verified = true; validations++
    return headers
  })
  t.mock.method(AbortSignal, 'timeout', milliseconds => {
    assert.equal(verified, true, 'no network deadline may start before trusted local validation')
    verified = false; budgets.push(milliseconds)
    return timeout.call(AbortSignal, milliseconds)
  })
  assert.equal((await adapter.isAvailable()).ready, true)
  assert.deepEqual(await adapter.listSessions(), [])
  assert.equal((await collect(adapter, { sessionId: 'a', text: 'synthetic' })).at(-1).kind, 'done')
  assert.deepEqual(budgets, [2000, 5000, 600000], 'existing network budgets stay unchanged')
  assert.equal(validations, 3)
  assert.equal(requests, 3)
})
test('unauthenticated health-style success is not readiness', async t => {
  let calls = 0
  const { adapter } = await fixture(t, (_, res) => { calls++; res.end(JSON.stringify({ ok: true, version: 2 })) })
  assert.equal((await adapter.isAvailable()).ready, false)
  assert.equal(calls, 1, 'the authorized endpoint must be reached before rejecting its readiness payload')
})
test('sessions keep compact display metadata and newest ordering', async t => {
  const { adapter } = await fixture(t, (_, res) => res.end(JSON.stringify({ version: 2, sessions: [
    { id: 'older', title: 'old', lastModified: 1 }, { id: 'newer', title: 'new', lastModified: 2, displayContent: 'display', state: 'stopped', transitionRevision: 3 }, { id: '' },
  ] })))
  const rows = await adapter.listSessions(); assert.deepEqual(rows.map(r => r.id), ['newer', 'older']); assert.equal(rows[0].displayContent, 'display'); assert.equal(rows[0].transitionRevision, 3)
})
test('inject sends exact session and random requestId once; admission remains admission', async t => {
  let calls = 0
  const { adapter } = await fixture(t, async (req, res) => {
    calls++; let body = ''; for await (const c of req) body += c
    const b = JSON.parse(body); assert.equal(b.sessionId, 'a'); assert.match(b.requestId, /^[a-f0-9-]{36}$/)
    sse(res, [['token', { text: 'answer' }], ['tool', { name: 'bash', phase: 'end', ok: false }], ['done', { sessionId: 'a', stopReason: 'admitted' }]])
  })
  const events = await collect(adapter, { sessionId: 'a', text: 'voice' }); assert.deepEqual(events.map(e => e.kind), ['token', 'tool', 'done']); assert.equal(events[2].stopReason, 'admitted'); assert.equal(calls, 1)
})
test('auto/new session cannot silently target the newest conversation', async t => {
  let calls = 0; const { adapter } = await fixture(t, (_, res) => { calls++; res.end() })
  assert.equal((await collect(adapter, { sessionId: 'auto', text: 'voice' }))[0].code, 'EXACT_SESSION_REQUIRED')
  await assert.rejects(adapter.openNew(), /explicit/); assert.equal(calls, 0)
})
test('stream EOF without receipt is delivery-unknown, never done', async t => {
  const { adapter } = await fixture(t, (_, res) => sse(res, [['token', { text: 'partial' }]]))
  assert.equal((await collect(adapter, { sessionId: 'a', text: 'voice' })).at(-1).code, 'DELIVERY_UNKNOWN')
})
test('mismatched target receipt and oversized unterminated frame are errors', async t => {
  let n = 0
  const { adapter } = await fixture(t, (_, res) => { if (++n === 1) sse(res, [['done', { sessionId: 'b' }]]); else { res.writeHead(200, { 'Content-Type': 'text/event-stream' }); res.end('x'.repeat(70000)) } })
  assert.equal((await collect(adapter, { sessionId: 'a', text: 'voice' }))[0].code, 'SESSION_MISMATCH')
  assert.equal((await collect(adapter, { sessionId: 'a', text: 'voice' }))[0].code, 'AGENT_STREAM_FAILED')
})
test('HTTP redirect is not followed and credentials do not leave endpoint', async t => {
  let calls = 0; const { adapter } = await fixture(t, (_, res) => { calls++; res.writeHead(302, { Location: 'http://evil.test/' }); res.end() })
  assert.equal((await collect(adapter, { sessionId: 'a', text: 'voice' }))[0].code, 'AGENT_UNAVAILABLE'); assert.equal(calls, 1)
})
test('unsafe descriptor and missing bootstrap fail closed without anonymous fallback', async t => {
  let calls = 0; const { adapter, file } = await fixture(t, (_, res) => { calls++; res.end() })
  if (process.platform === 'win32') {
    // A newly created child inherits the directory ACL but has no protected,
    // explicit file DACL. Keep valid synthetic descriptor bytes so rejection
    // proves the ACL gate, rather than malformed JSON or a missing endpoint.
    const descriptor = fs.readFileSync(file)
    fs.unlinkSync(file)
    fs.writeFileSync(file, descriptor, { mode: 0o600 })
    assert.throws(() => windowsAcl('verify-file', file))
  } else fs.chmodSync(file, 0o644)
  assert.equal((await adapter.isAvailable()).ready, false); assert.equal(calls, 0)
  fs.unlinkSync(file); assert.deepEqual(await adapter.listSessions(), []); assert.equal(calls, 0)
})
test('already-aborted request performs no network I/O', async t => {
  let calls = 0; const { adapter } = await fixture(t, (_, res) => { calls++; res.end() })
  const c = new AbortController(); c.abort(); const events = await collect(adapter, { sessionId: 'a', text: 'voice', signal: c.signal }); assert.equal(events[0].code, 'CANCELLED'); assert.equal(calls, 0)
})
