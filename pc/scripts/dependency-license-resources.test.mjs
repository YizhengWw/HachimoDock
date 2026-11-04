import test from 'node:test';
import assert from 'node:assert/strict';
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { copyDependencyLicenses, dependencyLicenseResources } from './dependency-license-resources.mjs';

const repository = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), 'dependency-license-test-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const source = join(root, 'licenses'), output = join(root, 'output');
  for (const name of ['pc/package-lock.json','pc/src-tauri/Cargo.lock','pc/src-tauri/bridge/packages/clawd-backend-service/package-lock.json']) {
    const destination=join(root,name);mkdirSync(dirname(destination),{recursive:true});copyFileSync(join(repository,name),destination);
  }
  mkdirSync(source);
  for (const { name } of dependencyLicenseResources) {
    copyFileSync(join(repository, 'licenses', name), join(source, name));
  }
  return { source, output };
}

test('installer stages all licenses and source notice with byte-identical content', t => {
  const { source, output } = fixture(t);
  const receipt = copyDependencyLicenses(source, output);
  assert.equal(Object.keys(receipt).length, dependencyLicenseResources.length);
  assert.deepEqual(readdirSync(output).sort(), dependencyLicenseResources.map(x => x.name).sort());
  for (const { name, sha256 } of dependencyLicenseResources) {
    assert.deepEqual(readFileSync(join(output, name)), readFileSync(join(source, name)));
    assert.equal(receipt[`licenses/${name}`], sha256);
  }
  assert.match(readFileSync(join(output, 'DEPENDENCY-LICENSE-NOTICE.md'), 'utf8'),
    /https:\/\/static\.crates\.io\/crates\/serialport\/serialport-4\.9\.0\.crate/);
});

test('tampered MPL text rejects before producing a partial license directory', t => {
  const { source, output } = fixture(t);
  writeFileSync(join(source, 'MPL-2.0.txt'), 'replaced license');
  assert.throws(() => copyDependencyLicenses(source, output), /integrity mismatch/);
  assert.equal(existsSync(output), false);
});

test('missing selected MIT grant rejects before producing a partial directory', t => {
  const { source, output } = fixture(t);
  rmSync(join(source, 'unescaper-0.1.8-MIT-LICENSE.txt'));
  assert.throws(() => copyDependencyLicenses(source, output));
  assert.equal(existsSync(output), false);
});

test('missing MiSans agreement blocks distribution instead of omitting the font license', t => {
  const { source, output } = fixture(t);
  rmSync(join(source, 'MiSans-Font-License-Agreement.pdf'));
  assert.throws(() => copyDependencyLicenses(source, output));
  assert.equal(existsSync(output), false);
});

test('modified MiSans attribution blocks distribution before writing any licenses', t => {
  const { source, output } = fixture(t);
  writeFileSync(join(source, 'MiSans-Embedded-Fonts-NOTICE.txt'), 'incomplete attribution');
  assert.throws(() => copyDependencyLicenses(source, output), /integrity mismatch/);
  assert.equal(existsSync(output), false);
});

test('source symlink is rejected even when it points to valid license bytes', t => {
  const { source, output } = fixture(t);
  const name = 'unescaper-0.1.8-MIT-LICENSE.txt';
  copyFileSync(join(source, name), join(source, 'alias.txt'));
  rmSync(join(source, name));
  symlinkSync(join(source, 'alias.txt'), join(source, name));
  assert.throws(() => copyDependencyLicenses(source, output), /Regular license/);
  assert.equal(existsSync(output), false);
});

test('conflicting staged license is preserved and blocks the whole operation', t => {
  const { source, output } = fixture(t);
  mkdirSync(output);
  writeFileSync(join(output, 'MPL-2.0.txt'), 'unexpected existing bytes');
  assert.throws(() => copyDependencyLicenses(source, output), /integrity mismatch/);
  assert.deepEqual(readdirSync(output), ['MPL-2.0.txt']);
  assert.equal(readFileSync(join(output, 'MPL-2.0.txt'), 'utf8'), 'unexpected existing bytes');
});

test('Mac/common Tauri resources positively map the same exact license files', () => {
  const config = JSON.parse(readFileSync(join(repository, 'pc/src-tauri/tauri.conf.json'), 'utf8'));
  for (const { name } of dependencyLicenseResources) {
    assert.equal(config.bundle.resources[`../../licenses/${name}`], `licenses/${name}`);
  }
});
