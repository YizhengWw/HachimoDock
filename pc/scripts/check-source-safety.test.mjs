import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir, homedir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const guard = readFileSync(new URL('./check-source-safety.mjs', import.meta.url));

function check(files) {
  const root = mkdtempSync(join(tmpdir(), 'publication-guard-'));
  try {
    mkdirSync(join(root, 'pc/scripts'), { recursive: true });
    writeFileSync(join(root, 'pc/scripts/check-source-safety.mjs'), guard);
    const init = spawnSync('git', ['init', '--quiet', root], { encoding: 'utf8' });
    assert.equal(init.status, 0, init.stderr);
    for (const [name, contents] of Object.entries(files)) {
      const path = join(root, name);
      mkdirSync(join(path, '..'), { recursive: true });
      writeFileSync(path, contents);
    }
    const run = spawnSync(process.execPath, ['pc/scripts/check-source-safety.mjs'], { cwd: root, encoding: 'utf8' });
    return { status: run.status, output: run.stdout + run.stderr };
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
}

test('a binary asset cannot bypass credential scanning with a null byte', () => {
  const token = 'glpat-' + 'a'.repeat(28);
  const result = check({ 'firmware.bin': Buffer.concat([Buffer.from([0, 1]), Buffer.from(token)]) });
  assert.equal(result.status, 1);
  assert.match(result.output, /potential credential; value redacted/);
  assert.ok(!result.output.includes(token));
});

test('binary assets cannot carry a developer home path', () => {
  const result = check({ 'firmware.bin': Buffer.from('\0' + homedir() + '/private-build/source.c') });
  assert.equal(result.status, 1);
  assert.match(result.output, /personal build path/);
});

test('private repository endpoints fail without reflecting the endpoint', () => {
  const host = 'git.n.' + 'example.invalid';
  const result = check({ 'notes.txt': 'https://' + host + '/project' });
  assert.equal(result.status, 1);
  assert.match(result.output, /private repository endpoint/);
  assert.ok(!result.output.includes(host));
});

test('Vite caches are rejected unless excluded from publication inputs', () => {
  assert.equal(check({ '.vite/deps/cache.js': 'export default 1;' }).status, 1);
  assert.equal(check({ '.gitignore': '.vite/\n', '.vite/deps/cache.js': 'export default 1;' }).status, 0);
});

test('ordinary binary resources and public service endpoints remain publishable', () => {
  const result = check({ 'image.bin': Buffer.from([0, 1, 2, 255]), 'service.js': 'export const endpoint = "https://example.org/api";' });
  assert.equal(result.status, 0, result.output);
});

test('public guides reject private delivery records but retain user API configuration instructions', () => {
  assert.equal(check({ 'pc/docs/example.md': '仅限内部' + '流转' }).status, 1);
  assert.equal(check({ 'pc/docs/example.md': '两项已授权' + ' Key 与两张企业' + ' CA' }).status, 1);
  const result = check({ 'pc/docs/example.md': '请在 API 配置中填写自己的服务 Key。' });
  assert.equal(result.status, 0, result.output);
});
