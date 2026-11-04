import test from 'node:test';
import assert from 'node:assert/strict';
import {options, plan, execute, counts, PYTHON_TESTS, SNAPSHOT_TREES, SNAPSHOT_FILES,snapshotInputs} from './test-framework-offline.mjs';
import {mkdtempSync,mkdirSync,writeFileSync,rmSync,symlinkSync,truncateSync} from 'node:fs';
import {join} from 'node:path';
import {tmpdir} from 'node:os';

const stage = (id, extra = {}) => ({id, selected: true, depends: [], kind: 'node', ...extra});
const success = {code: 0, counts: {passed: 3, failed: 0, skipped: 0}};
function fake({missing = [], results = {}} = {}) {
  const called = [];
  return {called, check: async s => ({ok: !missing.includes(s.id), reason: 'missing_dependency'}),
    run: async s => {called.push(s.id); return results[s.id] || success;}};
}
test('explicit output, recognized options and loopback port are mandatory', () => {
  assert.throws(() => options([], {}), /explicit/);
  assert.throws(() => options(['--output'], {}), /Missing/);
  assert.throws(() => options(['--output', '/tmp/out', '--flash'], {}), /Unknown/);
  assert.throws(() => options(['--output', '/tmp/out', '--port', '0'], {}), /port/);
  const c = options(['--output', '/tmp/out', '--python', '/tools/python', '--browser'], {PET_FRAMEWORK_CARGO: '/tools/cargo'});
  assert.equal(c.python, '/tools/python'); assert.equal(c.cargo, '/tools/cargo'); assert.equal(c.browser, true);
});
test('default plan contains only allowlisted offline paths, never full Rust tests or a device probe', () => {
  const c = options(['--output', '/tmp/synthetic'], {}), stages = plan(c, '/tmp/synthetic', '/synthetic/repo');
  assert.deepEqual(stages.filter(s => s.selected).map(s => s.id), ['runner_selftest', 'frontend', 'bridge', 'firmware_host', 'workflows', 'graphics', 'form_schema','evidence_tools']);
  assert(stages.find(s => s.id === 'frontend').args.includes('--offline'));
  assert(stages.find(s => s.id === 'firmware_host').args.includes('-m'));
  assert(PYTHON_TESTS.every(p => !p.includes('/') && p.split('::')[0].endsWith('_test.py')));
  assert(PYTHON_TESTS.includes('petui_runtime_mutex_test.py'));
  assert(PYTHON_TESTS.includes('p4_json_strings_test.py'));
  assert(PYTHON_TESTS.includes('p4_program_scheduler_test.py'));
  assert(PYTHON_TESTS.includes('p4_component_program_test.py'));
  assert(PYTHON_TESTS.includes('p4_program_publication_test.py'));
  assert(PYTHON_TESTS.includes('p4_collection_host_test.py'));
  assert(PYTHON_TESTS.includes('p4_host_collection_test.py'));
  assert(PYTHON_TESTS.includes('petui_storage_scope_owner_test.py'));
  assert(!PYTHON_TESTS.some(p => p.includes('test_runtime_p4_object_only')));
  assert.equal(stages.find(s => s.id === 'browser_ui').selected, false);
  assert.equal(stages.find(s=>s.id==='program_background_ui').selected,false);
  assert.equal(stages.find(s=>s.id==='program_controller').selected,false);
  assert.equal(stages.find(s=>s.id==='program_controller').scope,'exact_synthetic_usb_writer_tests_no_device_or_gui');
  assert.equal(stages.find(s=>s.id==='serial_control_integrity').selected,false);
  assert.equal(stages.find(s=>s.id==='serial_control_integrity').scope,'exact_raw_reader_control_and_legacy_tests_no_device_or_gui');
  assert(PYTHON_TESTS.includes('p4_control_integrity_test.py'));
  assert(PYTHON_TESTS.includes('p4_ppa_trace_test.py')&&PYTHON_TESTS.includes('p4_ppa_queue_test.py'));
  assert(PYTHON_TESTS.includes('p4_rgb565_rotate_test.py'));
  assert(PYTHON_TESTS.includes('p4_h264_pack_word_test.py'));
  assert(PYTHON_TESTS.includes('p4_mixed_profile_test.py'));
  assert.deepEqual(stages.find(s => s.id === 'browser_ui').depends, []);
  assert.equal(stages.find(s => s.id === 'desktop_typecheck').selected, false);
  assert.equal(stages.find(s => s.id === 'desktop_typecheck').scope, 'compile_only_no_bundle_validation');
  assert(SNAPSHOT_TREES.includes('pc/tests/manual'));
  assert(SNAPSHOT_FILES.includes('pc/src-tauri/Cargo.lock'));
  assert(SNAPSHOT_TREES.includes('pc/src-tauri/permissions')&&SNAPSHOT_TREES.includes('pc/src-tauri/capabilities'));
  for(const name of ['build.rs','tauri.conf.json','tauri.macos.conf.json','tauri.windows.conf.json','tauri.dev.conf.json'])assert(SNAPSHOT_FILES.includes('pc/src-tauri/'+name));
  assert(SNAPSHOT_FILES.includes('firmware/protocol.md'));
  assert(SNAPSHOT_FILES.includes('firmware/main/CMakeLists.txt'));
  assert(SNAPSHOT_TREES.includes('firmware/components')&&SNAPSHOT_TREES.includes('firmware/assets')&&SNAPSHOT_TREES.includes('firmware/scripts'));
  assert(SNAPSHOT_FILES.includes('firmware/factory-config.json'));
  assert.deepEqual(stages.find(s => s.id === 'desktop_typecheck').args.slice(0, 4), ['check', '--tests', '--offline', '--locked']);
  assert(!stages.flatMap(s => s.args).some(s => /--ignored|upload|flash|cargo test/.test(s)));
});
test('source evidence includes local BSP build recipes, binary assets and test vendors, with canonical framed hashes',()=>{
 const fixture=mkdtempSync(join(tmpdir(),'pet-source-snapshot-'));
 try{
  const tree=join(fixture,'tree');mkdirSync(tree);mkdirSync(join(tree,'vendor'));mkdirSync(join(tree,'__pycache__'));
  writeFileSync(join(tree,'main.c'),'source');writeFileSync(join(tree,'CMakeLists.txt'),'recipe');writeFileSync(join(tree,'sprite.bin'),Buffer.from([0,1,0,255]));writeFileSync(join(tree,'vendor','parser.c'),'third party');
  writeFileSync(join(tree,'__pycache__','ignored.pyc'),'cache');writeFileSync(join(tree,'.DS_Store'),'cache');
  const first=snapshotInputs(fixture,['tree'],['tree/CMakeLists.txt']);assert.equal(first.files,4);
  assert.deepEqual(first,snapshotInputs(fixture,['tree'],['tree/CMakeLists.txt']));
  writeFileSync(join(tree,'sprite.bin'),Buffer.from([0,1,0,254]));const asset=snapshotInputs(fixture,['tree'],[]);assert.notEqual(asset.digest,first.digest);
  writeFileSync(join(tree,'CMakeLists.txt'),'changed');const recipe=snapshotInputs(fixture,['tree'],[]);assert.notEqual(recipe.digest,asset.digest);
  writeFileSync(join(tree,'vendor','parser.c'),'changed');const vendor=snapshotInputs(fixture,['tree'],[]);assert.notEqual(vendor.digest,recipe.digest);
  writeFileSync(join(tree,'__pycache__','ignored.pyc'),'different cache');assert.deepEqual(snapshotInputs(fixture,['tree'],[]),vendor);
 }finally{rmSync(fixture,{recursive:true});}
});
test('source snapshot never follows symlinks, escaping paths, special roots or oversized build inputs',()=>{
 const fixture=mkdtempSync(join(tmpdir(),'pet-source-bounds-'));
 try{
  const root=join(fixture,'root');mkdirSync(root);mkdirSync(join(root,'tree'));writeFileSync(join(root,'tree','ok.c'),'source');writeFileSync(join(fixture,'outside.c'),'synthetic outside data');
  for(const p of ['../outside.c','..\\outside.c','/outside.c','tree/../ok.c','tree\0bad'])assert.throws(()=>snapshotInputs(root,[],[p]),/snapshot_path_invalid/);
  symlinkSync(join(fixture,'outside.c'),join(root,'tree','linked.c'));assert.throws(()=>snapshotInputs(root,['tree'],[]),/snapshot_symlink/);rmSync(join(root,'tree','linked.c'));
  symlinkSync(join(fixture,'outside.c'),join(root,'linked.c'));assert.throws(()=>snapshotInputs(root,[],['linked.c']),/snapshot_symlink/);rmSync(join(root,'linked.c'));
  writeFileSync(join(root,'tree','large.bin'),'');truncateSync(join(root,'tree','large.bin'),32*1024*1024+1);assert.throws(()=>snapshotInputs(root,['tree'],[]),/snapshot_file_size/);
 }finally{rmSync(fixture,{recursive:true});}
});
test('a build-time portable-lock expansion is visible, not normalized away by the evidence hash',()=>{
 const fixture=mkdtempSync(join(tmpdir(),'pet-source-lock-'));
 try{
  const file=join(fixture,'dependencies.lock'),portable='path: $PET_P4_PROJECT_DIR/components/local\n';
  writeFileSync(file,portable);const before=snapshotInputs(fixture,[],['dependencies.lock']);
  writeFileSync(file,portable.replace('$PET_P4_PROJECT_DIR',fixture));const expanded=snapshotInputs(fixture,[],['dependencies.lock']);
  assert.notEqual(before.digest,expanded.digest);
  writeFileSync(file,portable);assert.deepEqual(snapshotInputs(fixture,[],['dependencies.lock']),before);
 }finally{rmSync(fixture,{recursive:true});}
});
test('all requested stages may pass offline but never certify hardware, acoustics or release', async () => {
  const r = await execute([stage('one'), stage('two')], fake());
  assert.equal(r.status, 'offline_passed'); assert.equal(r.exitCode, 0); assert.equal(r.offlinePassed, true);
  assert.equal(r.passed, false); assert.equal(r.releaseReady, false);
  assert.equal(r.hardware.status, 'pending'); assert.equal(r.acoustic.status, 'pending');
});
test('non-selected optional stages stay skipped, never counted as all-offline success', async () => {
  const f = fake(), r = await execute([stage('one'), stage('optional', {selected: false})], f);
  assert.deepEqual(f.called, ['one']); assert.equal(r.status, 'incomplete');
  assert.equal(r.stages[1].status, 'skipped'); assert.equal(r.offlinePassed, false);
  assert.equal(r.requestedStagesPassed, true); assert.equal(r.exitCode, 0); assert.equal(r.passed, false);
});
test('one failing stage blocks dependents but independent stages continue', async () => {
  const f = fake({results: {first: {code: 1, output: 'failed'}}});
  const r = await execute([stage('first'), stage('child', {depends: ['first']}), stage('independent')], f);
  assert.deepEqual(f.called, ['first', 'independent']);
  assert.deepEqual(r.stages.slice(0, 3).map(s => s.status), ['failed', 'blocked', 'passed']);
  assert.equal(r.status, 'failed'); assert.equal(r.exitCode, 1);
});
test('fail-fast records remaining phases as blocked rather than disappearing or passing', async () => {
  const f = fake({results: {first: {code: 4}}});
  const r = await execute([stage('first'), stage('next')], f, {failFast: true});
  assert.deepEqual(f.called, ['first']); assert.equal(r.stages[1].reason, 'fail_fast'); assert.equal(r.exitCode, 1);
});
test('missing tool is blocked, not skipped as success; independent work still runs', async () => {
  const f = fake({missing: ['missing']});
  const r = await execute([stage('missing'), stage('other')], f);
  assert.deepEqual(f.called, ['other']); assert.equal(r.stages[0].status, 'blocked');
  assert.equal(r.exitCode, 2); assert.equal(r.requestedStagesPassed, false);
});
test('ENOENT, offline cache absence, zero tests and malformed reports cannot pass', async () => {
  for (const result of [{code: -2, error: 'ENOENT'}, {code: 1, blocked: 'missing_offline_dependency'},
    {code: 0, counts: {passed: 0, failed: 0, skipped: 0}}, {code: 0, output: 'nothing verified'}]) {
    const r = await execute([stage('x')], fake({results: {x: result}}));
    assert.equal(r.stages[0].status, 'blocked'); assert.equal(r.exitCode, 2); assert.equal(r.offlinePassed, false);
  }
});
test('exit zero with a failed or skipped summary is not a passed stage', async () => {
  for (const [summary, status, code] of [[{passed: 3, failed: 1, skipped: 0}, 'failed', 1],
    [{passed: 3, failed: 0, skipped: 1}, 'skipped', 2]]) {
    const r = await execute([stage('x')], fake({results: {x: {code: 0, counts: summary}}}));
    assert.equal(r.stages[0].status, status); assert.equal(r.exitCode, code); assert.equal(r.requestedStagesPassed, false);
  }
});
test('timeout or signal cannot reuse a prior successful summary', async () => {
  for (const result of [{...success, error: 'timeout'}, {...success, signal: 'SIGTERM'}]) {
    const r = await execute([stage('x')], fake({results: {x: result}}));
    assert.equal(r.stages[0].status, 'failed'); assert.equal(r.exitCode, 1);
  }
});
test('source snapshot failure blocks execution and a changing source invalidates the run', async () => {
  const f = fake();
  let r = await execute([stage('x')], f, {source: () => {throw Error('unavailable');}});
  assert.deepEqual(f.called, []); assert.equal(r.exitCode, 2);
  let revision = 0;
  r = await execute([stage('x')], fake(), {source: () => ({valid: true, digest: String(revision++)})});
  assert.equal(r.stages.at(-1).reason, 'source_changed_or_unavailable'); assert.equal(r.exitCode, 2);
});
test('partial reports retain each executed, skipped and blocked result', async () => {
  const snapshots = [];
  const r = await execute([stage('x'), stage('off', {selected: false})], fake(), {save: s => snapshots.push(structuredClone(s))});
  assert.equal(snapshots.length, 4); assert.equal(snapshots[0].stages.length, 2);
  assert.equal(snapshots[0].stages[0].reason, 'not_started');
  assert.deepEqual(snapshots.at(-1), r); assert.equal(r.passed, false);
});
test('Node, Rust, pytest and browser summaries count skipped cases explicitly', () => {
  assert.deepEqual(counts('node', 'ℹ tests 5\nℹ pass 3\nℹ fail 0\nℹ cancelled 0\nℹ skipped 1\nℹ todo 1'), {passed: 3, failed: 0, skipped: 2});
  assert.deepEqual(counts('rust', 'test result: ok. 4 passed; 0 failed; 2 ignored;\ntest result: ok. 0 passed; 0 failed; 0 ignored;'), {passed: 4, failed: 0, skipped: 2});
  assert.deepEqual(counts('pytest', '', '<testsuites><testsuite tests="5" failures="1" errors="1" skipped="1"></testsuite></testsuites>'), {passed: 2, failed: 2, skipped: 1});
  assert.deepEqual(counts('browser', '', JSON.stringify({passed: 1, results: [{passed: true}], pageErrors: [], blockedExternalRequests: []})), {passed: 1, failed: 0, skipped: 0});
  assert.throws(() => counts('browser', '', JSON.stringify({passed: 1, results: [{passed: true}], blockedExternalRequests: ['https://external']})));
});
