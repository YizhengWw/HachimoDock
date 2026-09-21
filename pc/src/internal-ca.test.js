import { test } from 'node:test';
import assert from 'node:assert/strict';
import { X509Certificate } from 'node:crypto';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { selectApprovedCa } from '../scripts/prepare-internal-ca.mjs';
import { parseInternalCredentials } from '../scripts/prepare-internal-credentials.mjs';

test('internal CA selection is fingerprint-bound, current, public-only and minimal', () => {
  const dir = mkdtempSync(join(tmpdir(), 'pet-ca-test-'));
  try {
    execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
      '-subj', '/CN=Test CA', '-keyout', join(dir, 'key.pem'), '-out', join(dir, 'ca.pem'),
      '-addext', 'basicConstraints=critical,CA:TRUE'], { stdio: 'ignore' });
    const raw = readFileSync(join(dir, 'ca.pem'), 'utf8');
    const cert = new X509Certificate(raw);
    const fp = cert.fingerprint256;
    assert.equal(new X509Certificate(selectApprovedCa(raw, fp)).fingerprint256, fp);
    assert.throws(() => selectApprovedCa(raw, ''));
    assert.throws(() => selectApprovedCa(raw, '0'.repeat(64)));
    assert.throws(() => selectApprovedCa(raw, `${fp},${fp}`));
    assert.throws(() => selectApprovedCa(raw + raw, fp));
    assert.throws(() => selectApprovedCa(raw + readFileSync(join(dir, 'key.pem'), 'utf8'), fp));
    assert.throws(() => selectApprovedCa(raw, fp, new Date('2099-01-01')));
    assert.throws(() => selectApprovedCa(raw, fp, new Date('2000-01-01')));
    assert.throws(() => selectApprovedCa(' '.repeat(2 * 1024 * 1024 + 1), fp));
    // An unapproved certificate in the supplied bundle never enters the result.
    execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
      '-subj', '/CN=Other CA', '-keyout', join(dir, 'other-key.pem'), '-out', join(dir, 'other.pem'),
      '-addext', 'basicConstraints=critical,CA:TRUE'], { stdio: 'ignore' });
    assert.equal(selectApprovedCa(raw + readFileSync(join(dir, 'other.pem'), 'utf8'), fp), cert.toString());
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test('public web build rejects internal CA inputs even without credentials', () => {
  for (const name of ['PET_MANAGER_INTERNAL_CA_FILE', 'PET_MANAGER_INTERNAL_CA_SHA256', 'PET_MANAGER_INTERNAL_CREDENTIALS_FILE']) {
    const child = spawnSync(process.execPath, ['scripts/check-public-build-env.mjs'], {
      cwd: new URL('..', import.meta.url), env: { ...process.env, [name]: 'test-only' }, encoding: 'utf8',
    });
    assert.notEqual(child.status, 0);
    assert.match(child.stderr, new RegExp(name));
  }
});

test('internal credentials allow only two explicit keys, with no proxy or endpoint injection', () => {
  const value = { arkApiKey: 'test-ark-key-for-build', speechApiKey: 'test-speech-key-for-build' };
  assert.deepEqual(parseInternalCredentials(JSON.stringify(value)), value);
  for (const invalid of [{}, { ...value, proxy: 'http://localhost' }, { ...value, arkApiKey: 'bad\nkey' }]) {
    assert.throws(() => parseInternalCredentials(JSON.stringify(invalid)));
  }
  assert.throws(() => parseInternalCredentials('{not-json'));
  const publicVite = readFileSync(new URL('../vite.config.js', import.meta.url), 'utf8');
  assert.doesNotMatch(publicVite, /readInternalCredentials|PET_MANAGER_INTERNAL_CREDENTIALS_FILE/);
  assert.match(publicVite, /__PET_MANAGER_INTERNAL_CONTENT_API_KEY__: JSON.stringify\(\s*""/);
});
