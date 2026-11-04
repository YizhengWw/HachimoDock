// Small production-source Rust harness. No Tauri host, device, credentials or
// HTTP transport is initialized. Cargo must use an already-populated cache.
// Usage: node scripts/test-component-workflows-offline.mjs EVIDENCE_DIR LABEL [FILTER]
import {readFileSync, writeFileSync, mkdirSync, mkdtempSync} from 'node:fs';
import {resolve, dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';

const pc = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const [output, label = 'run', filter] = process.argv.slice(2);
if (!output || !/^[a-zA-Z0-9_-]+$/.test(label)) throw new Error('Provide an evidence directory and safe label.');
const evidence = resolve(output), source = join(pc, 'src-tauri/src');
mkdirSync(evidence, {recursive: true});
const root = mkdtempSync(join(evidence, `workflow-${label}-`));
const read = name => readFileSync(join(source, name), 'utf8');
function section(text, from, to) {
  const start = text.indexOf(from), end = text.indexOf(to, start + from.length);
  if (start < 0 || end < 0) throw new Error(`Production boundary changed: ${from}`);
  return text.slice(start, end);
}
const declarations = read('app_services.rs');
// The exact Declaration and composition dispatch branch, not a reimplemented
// validator. Non-composition services deliberately cannot be reached here.
const declaration = section(declarations, '#[derive(Clone,Debug,Deserialize,Serialize)]', 'impl Declaration');
const parse = section(declarations, 'pub fn parse(', '    let library=d.is_library();') + '    Err(error())\n}\n';
const atomic = section(read('component_app.rs'), 'pub(crate) fn atomic(', 'fn load(');
const validUrl = section(read('media_network.rs'), 'pub fn valid_url(', 'pub fn get(');
const modules = ['json_canonical', 'component_composition', 'component_collection', 'component_expr', 'component_ledger', 'component_runtime_lifecycle', 'component_external_outcome', 'app_canvas', 'app_host_collection', 'app_component_program', 'app_program_background', 'app_variable_owners'];
// A #[path] import changes Rust's nested-module search directory. Point this
// one child explicitly at the production file; no executable body is changed.
const lifecycle = read('component_runtime_lifecycle.rs');
if (!lifecycle.includes('mod resource_broker;')) throw new Error('Lifecycle module boundary changed');
writeFileSync(join(root, 'component_runtime_lifecycle.rs'), lifecycle.replace('mod resource_broker;',
  `#[path=${JSON.stringify(join(source, 'component_runtime_lifecycle/resource_broker.rs'))}] mod resource_broker;`));
const imports = modules.map(name => `#[path=${JSON.stringify(join(name === 'component_runtime_lifecycle' ? root : source, `${name}.rs`))}] mod ${name};`).join('\n');
writeFileSync(join(root, 'Cargo.toml'), `[package]
name = "component-workflow-offline-check"
version = "0.0.0"
edition = "2021"
[lib]
path = "lib.rs"
[dependencies]
serde = {version="1",features=["derive"]}
serde_json = {version="1",features=["preserve_order"]}
sha2 = "0.10"
chrono = "0.4"
tempfile = "3.27.0"
libc = "0.2"
url = "2.5"
[profile.dev]
debug = 0
incremental = false
`);
writeFileSync(join(root, 'lib.rs'), `#![allow(dead_code, unused_imports)]
${imports}
mod location_profile { pub type Profile = serde_json::Value; }
// Non-composition routes are deliberately unreachable in this extraction harness.
mod pc_fetch {
 pub const ENGINE:&str="pc-fetch-v1";
 pub fn parse_declaration(_: &[u8],_: &serde_json::Value)->Result<(),String>{panic!("Fetch is outside the composition-only offline harness");}
}
mod service_bridge {
 pub const ENGINE:&str="pc-service-bridge-v1";
 pub mod native {pub fn parse(_: &crate::app_services::Declaration,_:&serde_json::Value)->Result<(),String>{panic!("Service bridge is outside composition-only harness");}}
}
mod app_services {
use serde::{Deserialize, Serialize}; use serde_json::Value; use std::collections::BTreeMap;
${declaration}
${parse}
}
mod component_app {
use serde::Serialize; use std::path::Path; use std::io::Write;
${atomic}
}
mod media_network {
use url::Url;
${validUrl}
pub fn scoped_json_https(_: &str, _: &[String], _: usize) -> Result<serde_json::Value, String> {
    panic!("Real network is prohibited in the offline workflow harness");
}
}
`);
const args = ['test', '--offline', '--manifest-path', join(root, 'Cargo.toml')];
if (filter) args.push(filter);
args.push('--', '--test-threads=1');
const result = spawnSync(process.env.PET_WORKFLOW_CARGO || 'cargo', args, {
  cwd: pc, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024,
  env: {...process.env, CARGO_TARGET_DIR: process.env.PET_WORKFLOW_TARGET_DIR || join(evidence, 'rust-target')},
});
const log = `${result.stdout || ''}\n${result.stderr || ''}${result.error ? `\n${result.error.message}` : ''}`;
writeFileSync(join(evidence, `${label}.log`), log);
const inputs = [...modules.map(name => `${name}.rs`), 'component_external_outcome/file_journal.rs', 'component_runtime_lifecycle/resource_broker.rs', 'app_services.rs', 'component_app.rs', 'media_network.rs'];
const report = {
  label, filter: filter || null, status: result.status, generatedHarness: root,
  actualModules: modules, actualExtractedFunctions: ['Declaration', 'app_services::parse composition branch', 'component_app::atomic', 'media_network::valid_url'],
  stubs: ['location_profile::Profile (unreachable for composition)', 'HTTP transport (panics if called)'],
  cargoOffline: true, userDataRead: false, deviceUsed: false, audioUsed: false,
  sourceSha256: Object.fromEntries(inputs.map(name => [name, createHash('sha256').update(read(name)).digest('hex')])),
};
writeFileSync(join(evidence, `${label}.json`), JSON.stringify(report, null, 2) + '\n');
process.stdout.write(log);
process.exitCode = result.status ?? 1;
