// Production screenshot integrity/pull/event collector and rejected-line metadata; no serial, IPC,
// application launch, model service or user data. Uses an existing Cargo cache.
import {readFileSync, writeFileSync, mkdirSync, mkdtempSync} from 'node:fs';
import {resolve, dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';

const pc = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const [output, label = 'run'] = process.argv.slice(2);
if (!output || !/^[\w-]+$/.test(label)) throw Error('Provide evidence directory and safe label');
const evidence = resolve(output), source = join(pc, 'src-tauri/src');
mkdirSync(evidence, {recursive: true});
const root = mkdtempSync(join(evidence, `graphics-${label}-`));
const modules = {integrity: 'graphics_capture_integrity.rs', pull: 'graphics_capture_pull.rs', events: 'graphics_capture_events.rs',identity:'graphics_capture_identity.rs', serial_receive:'usb_serial/receive_diagnostics.rs'};
writeFileSync(join(root, 'Cargo.toml'), `[package]
name="graphics-capture-offline-check"
version="0.0.0"
edition="2021"
[lib]
path="lib.rs"
[dependencies]
base64="0.22"
serde={version="1",features=["derive"]}
serde_json="1"
tempfile="3.27.0"
[profile.dev]
debug=0
incremental=false
`);
writeFileSync(join(root, 'lib.rs'), '#![allow(dead_code)]\n' + Object.entries(modules)
  .map(([name, file]) => `#[path=${JSON.stringify(join(source, file))}] mod ${name};`).join('\n') + '\n');
const args = ['test', '--offline', '--manifest-path', join(root, 'Cargo.toml'), '--', '--test-threads=1'];
const result = spawnSync(process.env.PET_GRAPHICS_CARGO || 'cargo', args, {
  cwd: pc, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024,
  env: {...process.env, CARGO_TARGET_DIR: process.env.PET_GRAPHICS_TARGET_DIR || join(evidence, 'rust-target')},
});
const log = `${result.stdout || ''}\n${result.stderr || ''}${result.error ? `\n${result.error.message}` : ''}`;
writeFileSync(join(evidence, `${label}.log`), log);
writeFileSync(join(evidence, `${label}.json`), JSON.stringify({status: result.status, generatedHarness: root,
  actualModules: Object.values(modules), stubs: [], cargoOffline: true,
  nativeSerialTestIncluded: false, deviceUsed: false, userDataRead: false, audioUsed: false,
  sourceSha256: Object.fromEntries([...Object.values(modules), 'graphics_acceptance.rs'].map(name =>
    [name, createHash('sha256').update(readFileSync(join(source, name))).digest('hex')])),
}, null, 2) + '\n');
process.stdout.write(log); process.exitCode = result.status ?? 1;
