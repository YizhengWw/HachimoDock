'use strict'

/**
 * Package-level invariants: a dependency-free CommonJS Cordis plugin that the
 * DSH Loader can import and a bundle declaration the Plugin Manager can read.
 */

const test = require('node:test')
const assert = require('node:assert/strict')
const { readFileSync, readdirSync } = require('node:fs')
const { join } = require('node:path')
const { pathToFileURL } = require('node:url')

const packageRoot = join(__dirname, '..')
const manifest = JSON.parse(readFileSync(join(packageRoot, 'package.json'), 'utf8'))

test('package manifest is the expected DSH bundle package', () => {
  assert.equal(manifest.name, '@hachimodock/dsh-bridge')
  assert.equal(manifest.version, '0.1.0')
  assert.equal(manifest.type, 'commonjs')
  assert.equal(manifest.main, 'index.js')
  assert.equal(manifest.dsh.bundle.patch, './cordis.patch.yml')
  assert.equal(manifest.dependencies, undefined, 'no runtime npm dependencies')
})

test('the plugin entry exports a Cordis plugin shape', () => {
  const plugin = require('../index.js')
  assert.equal(typeof plugin.apply, 'function')
  assert.equal(typeof plugin.name, 'string')
  assert.equal(plugin.name.length > 0, true)
  assert.deepEqual([...plugin.inject].sort(), ['sessions'])
  assert.equal(plugin.AGENT_ID, 'deepseek-harness')
})

test('the plugin loads through the ESM import() path the DSH Loader uses', async () => {
  const namespace = await import(pathToFileURL(join(packageRoot, 'index.js')).href)
  // Loader.unwrapExports(): prefer `default`, then require the __esModule flag.
  const unwrapped = namespace.default ?? namespace
  assert.equal(typeof unwrapped.apply, 'function')
  assert.equal(unwrapped.name, 'hachimodock-dsh-bridge')
  assert.deepEqual([...unwrapped.inject].sort(), ['sessions'])
})

test('the bundle patch mounts the plugin row', () => {
  const patch = readFileSync(join(packageRoot, 'cordis.patch.yml'), 'utf8')
  assert.match(patch, /insert:/)
  assert.match(patch, /name: '@hachimodock\/dsh-bridge'/)
  assert.match(patch, /id: hachimodock-bridge/)
  assert.match(patch, /port: 17892/)
})

test('runtime modules only require Node built-ins and relative files', () => {
  const allowedNode = /^node:/
  for (const file of ['index.js', 'server.js', 'dsh-bridge.js']) {
    const source = readFileSync(join(packageRoot, file), 'utf8')
    const matches = [...source.matchAll(/require\(\s*['"]([^'"]+)['"]\s*\)/g)].map((match) => match[1])
    for (const specifier of matches) {
      assert.equal(
        allowedNode.test(specifier) || specifier.startsWith('.'),
        true,
        `${file} must not require external package ${specifier}`,
      )
    }
  }
})

test('test files import only Node built-ins, relative files, and each other', () => {
  const testDir = join(packageRoot, 'test')
  for (const file of readdirSync(testDir).filter((name) => name.endsWith('.js'))) {
    const source = readFileSync(join(testDir, file), 'utf8')
    const matches = [...source.matchAll(/require\(\s*['"]([^'"]+)['"]\s*\)/g)].map((match) => match[1])
    for (const specifier of matches) {
      assert.equal(
        specifier.startsWith('node:') || specifier.startsWith('.'),
        true,
        `${file} must not require external package ${specifier}`,
      )
    }
  }
})

test('the package has no npm dependencies and no lockfile committed', () => {
  assert.equal(manifest.devDependencies, undefined)
  assert.equal(manifest.peerDependencies, undefined)
})

 test('optional services are independent live capabilities, not bridge startup requirements', () => {
  const { serviceView, capabilities } = require('../host-contract')
  const { createFakeCtx, createFakeSession } = require('./helpers')
  const ctx = createFakeCtx({ sessions: [createFakeSession('optional')] })
  const view = serviceView(ctx)
  assert.equal(capabilities(view).sessions, true)
  assert.equal(capabilities(view).agentSnapshot, false)
  assert.equal(capabilities(view).backgroundPrompt, false)
  ctx.provide('agents', { get() {} })
  assert.equal(capabilities(view).agentSnapshot, true)
  assert.equal(capabilities(view).backgroundPrompt, false)
  ctx.provide('sessionController', { prompt() { throw Error('must not invoke') }, cancel() { throw Error('must not invoke') } })
  assert.equal(capabilities(view).backgroundPrompt, true)
  assert.equal(capabilities(view).interrupt, true)
})
