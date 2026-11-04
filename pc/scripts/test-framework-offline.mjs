// Unified offline acceptance. Never enumerate devices, initialize the client,
// run ignored Rust tests, download dependencies, package or publish artifacts.
import {readFileSync, readdirSync, statSync, lstatSync, openSync, closeSync, fstatSync, readSync, constants, mkdirSync, mkdtempSync, writeFileSync} from 'node:fs';
import {resolve, dirname, join, relative, isAbsolute, delimiter,sep} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawn, spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';

const pcRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repoRoot = dirname(pcRoot);
export const SNAPSHOT_TREES = ['pc/src', 'pc/src-tauri/src', 'pc/scripts', 'pc/skills/petui',
  'pc/tests/manual', 'firmware/main', 'firmware/tests','firmware/tools','firmware/boards',
  'firmware/components','firmware/scripts','firmware/assets',
  'pc/src-tauri/permissions','pc/src-tauri/capabilities'];
export const SNAPSHOT_FILES = ['pc/package.json', 'pc/package-lock.json', 'pc/src-tauri/Cargo.toml',
  'pc/src-tauri/Cargo.lock', 'firmware/CMakeLists.txt', 'firmware/main/CMakeLists.txt',
  'firmware/platformio.ini', 'firmware/protocol.md','firmware/sdkconfig.defaults','firmware/sdkconfig.v3.defaults',
  'firmware/dependencies.lock','firmware/dependencies.v3.lock','firmware/partitions.csv','firmware/factory-config.json',
  'pc/src-tauri/build.rs','pc/src-tauri/tauri.conf.json','pc/src-tauri/tauri.macos.conf.json',
  'pc/src-tauri/tauri.windows.conf.json','pc/src-tauri/tauri.dev.conf.json'];
// Reviewed host-only tests. Do not discover arbitrary new device tests here.
export const PYTHON_TESTS = [
  'p4_resources_test.py', 'p4_trusted_services_test.py', 'p4_future_admission_test.py',
  'p4_program_test.py::test_program_native_sanitizers',
  'p4_program_test.py::test_program_core_has_no_ambient_effects',
  'p4_program_test.py::test_native_evidence_rejects_sanitizer_reports_even_with_zero_exit',
  'p4_program_runtime_test.py::test_runtime_native_cases',
  'p4_program_runtime_test.py::test_runtime_native_fuzz',
  'p4_program_runtime_test.py::test_runtime_no_ambient_effects_and_still_disabled',
  'p4_program_probe_test.py', 'p4_program_main_probe_test.py', 'p4_program_reviewed_profile_test.py',
  'p4_debug_ingress_test.py', 'p4_program_host_test.py', 'p4_program_session_probe_test.py', 'p4_program_shared_session_test.py', 'p4_program_deadline_test.py', 'p4_component_program_test.py', 'p4_program_publication_test.py', 'p4_program_group_test.py', 'p4_program_scheduler_test.py', 'p4_program_store_test.py',
  'p4_program_preview_test.py', 'p4_program_output_contract_test.py', 'p4_program_timers_test.py', 'p4_program_cooldown_test.py', 'p4_renderer_stack_budget_test.py',
  'p4_event_bus_test.py', 'p4_observation_test.py', 'p4_collection_test.py', 'p4_collection_host_test.py', 'p4_host_collection_test.py', 'p4_external_outcome_boundary_test.py',
  'firmware_budget_test.py', 'platformio_portable_lock_test.py',
  'petui_storage_lane_test.py', 'petui_storage_scope_owner_test.py', 'petui_storage_mutation_test.py', 'petui_spare_package_write_test.py',
  'p4_bounded_file_read_test.py', 'p4_unchanged_install_test.py', 'p4_storage_cache_policy_test.py', 'p4_spiffs_hint_test.py', 'p4_catalog_profile_test.py', 'p4_program_registry_test.py', 'p4_program_parent_test.py', 'p4_background_protocol_test.py', 'p4_font_bitmap_test.py', 'p4_font_metrics_test.py', 'petui_cold_activation_test.py', 'petui_activation_files_test.py',
  'p4_background_projection_test.py', 'p4_main_lifecycle_clock_test.py', 'p4_storage_io_trace_test.py', 'p4_mixed_profile_test.py',
  'p4_native_work_window_test.py', 'p4_storage_partial_test.py', 'p4_storage_completed_test.py', 'p4_app_cache_budget_test.py', 'p4_app_cache_progressive_test.py',
  'p4_logical_frame_test.py', 'p4_panel_mirror_test.py', 'p4_save_trace_split_test.py',
  'p4_logical_retention_test.py', 'p4_storage_sparse_test.py', 'p4_glyph_axis_test.py',
  'petui_app_cache_test.py', 'petui_app_store_test.py', 'petui_save_capture_test.py', 'petui_save_schema_test.py', 'p4_save_stack_test.py', 'p4_state_memory_test.py',
  'p4_activation_worker_test.py', 'petui_runtime_mutex_test.py', 'petui_scene_prepare_test.py',
  'p4_host_pointer_identity_test.py', 'petui_resource_lifecycle_test.py', 'petui_app_host_test.py',
  'petui_render_snapshot_test.py', 'p4_variable_owners_test.py', 'p4_render_trace_test.py', 'p4_app_draw_trace_test.py', 'p4_ppa_trace_test.py', 'p4_ppa_queue_test.py', 'p4_lcd_split_test.py', 'p4_rgb565_span_test.py', 'p4_raster_exact_test.py', 'p4_alpha_fast_test.py', 'p4_spare_exact_reuse_test.py', 'p4_glyph_rows_test.py', 'p4_rgb565_rotate_test.py', 'p4_h264_pack_word_test.py', 'p4_h264_prefetch_test.py', 'p4_task_policy_test.py', 'p4_h264_replay_test.py', 'p4_render_reference_test.py',
  'p4_canvas_test.py',
  'p4_form_v2_test.py', 'p4_form_v1_preview_test.py',
  'p4_audio_realtime_test.py', 'p4_transport_boundaries_test.py', 'p4_control_integrity_test.py', 'p4_control_session_test.py', 'p4_query_reply_test.py', 'p4_unsolicited_hello_test.py', 'p4_reset_record_test.py', 'p4_reset_cost_test.py', 'p4_storage_snapshot_test.py', 'p4_native_erase_phase_test.py', 'p4_native_panic_test.py', 'p4_ota_observation_test.py', 'p4_uart_paced_reference_test.py', 'p4_native_asset_gate_test.py',
  'p4_audio_frontend_test.py::test_platformio_firmware_version_matches_cmake_release_version',
  'petui_navigation_test.py', 'p4_navigation_preview_test.py', 'petui_form_test.py', 'petui_viewport_test.py',
  'petui_runtime_semantics_test.py', 'petui_composition_test.py', 'petui_workflow_test.py', 'p4_widget_data_test.py',
  'p4_screenshot_integrity_test.py', 'p4_rx_frame_integrity_test.py', 'p4_json_strings_test.py',
  'p4_evidence_identity_test.py',
];

export function options(argv, env = process.env) {
  const config = {browser: false, desktopCheck: false, failFast: false, port: 4198,
    node: env.PET_FRAMEWORK_NODE || process.execPath,
    npm: env.PET_FRAMEWORK_NPM || 'npm', python: env.PET_FRAMEWORK_PYTHON || 'python3',
    cargo: env.PET_FRAMEWORK_CARGO || 'cargo', git: env.PET_FRAMEWORK_GIT || 'git',
    browserExecutable: env.PET_TEST_BROWSER, targetDir: env.PET_FRAMEWORK_TARGET_DIR};
  const values = {'--output': 'output', '--node': 'node', '--npm': 'npm', '--python': 'python',
    '--cargo': 'cargo', '--git': 'git', '--browser-executable': 'browserExecutable',
    '--target-dir': 'targetDir', '--port': 'port'};
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--browser') config.browser = true;
    else if (arg === '--desktop-check') config.desktopCheck = true;
    else if (arg === '--fail-fast') config.failFast = true;
    else if (arg in values) {
      if (!argv[i + 1] || argv[i + 1].startsWith('--')) throw Error(`Missing value for ${arg}`);
      config[values[arg]] = argv[++i];
    } else throw Error(`Unknown argument: ${arg}`);
  }
  if (!config.output) throw Error('An explicit --output directory is required');
  config.output = resolve(config.output);
  if (config.targetDir) config.targetDir = resolve(config.targetDir);
  config.port = Number(config.port);
  if (!Number.isInteger(config.port) || config.port < 1024 || config.port > 65535) throw Error('Invalid loopback port');
  return config;
}

export function plan(config, output, repo = repoRoot) {
  const pc = join(repo, 'pc');
  const stage = (id, command, args, extra = {}) => ({id, selected: true, depends: [],
    command, args, cwd: pc, timeoutMs: 600000, ...extra});
  return [
    stage('runner_selftest', config.node, ['--test', 'scripts/test-framework-offline.test.mjs'],
      {kind: 'node', needs: [[config.node, ['--version']]]}),
    stage('frontend', config.npm, ['--offline', '--ignore-scripts', 'run', 'test'],
      {kind: 'node', needs: [[config.npm, ['--version']]], requiredFiles: ['pc/node_modules/vite/package.json']}),
    stage('bridge', config.npm, ['--offline', '--ignore-scripts', 'run', 'test:bridge'],
      {kind: 'node', needs: [[config.npm, ['--version']]]}),
    stage('firmware_host', config.python, ['-m', 'pytest', '-q',
      ...PYTHON_TESTS.map(name => join('firmware/tests', name)),
      `--junitxml=${join(output, 'firmware-host.xml')}`],
      {cwd: repo, kind: 'pytest', junit: join(output, 'firmware-host.xml'),
        needs: [[config.python, ['-c', 'import pytest']], ['cc', ['--version']]]}),
    stage('workflows', config.node, ['scripts/test-component-workflows-offline.mjs', join(output, 'workflows'), 'run'],
      {kind: 'rust', needs: [[config.cargo, ['--version']]]}),
    stage('graphics', config.node, ['scripts/test-graphics-capture-offline.mjs', join(output, 'graphics'), 'run'],
      {kind: 'rust', needs: [[config.cargo, ['--version']]]}),
    stage('form_schema', config.node, ['scripts/test-form-schema-offline.mjs', join(output, 'forms'), 'run'],
      {kind: 'rust', needs: [[config.cargo, ['--version']]]}),
    stage('evidence_tools', config.node, ['--test', 'scripts/build-framework-evidence.test.mjs', 'scripts/verify-framework-evidence.test.mjs'],
      {kind: 'node', needs: [[config.node, ['--version']]]}),
    stage('browser_ui', config.node, ['scripts/test-framework-ui-offline.mjs', join(output, 'ui'), 'run'],
      {kind: 'browser', selected: config.browser, timeoutMs: 90000,
        requiredFiles: ['pc/node_modules/vite/bin/vite.js', 'pc/node_modules/playwright/package.json'],
        browserReport: join(output, 'ui/run.json')}),
    stage('program_background_ui',config.node,['scripts/test-program-background-ui-offline.mjs',join(output,'program-ui'),'run'],
      {kind:'browser',selected:config.browser,timeoutMs:90000,
        requiredFiles:['pc/node_modules/vite/bin/vite.js','pc/node_modules/playwright/package.json'],browserReport:join(output,'program-ui/run.json')}),
    stage('program_controller',config.node,['scripts/test-background-controller-offline.mjs',join(output,'program-controller'),'run'],
      {kind:'rust',selected:config.desktopCheck,timeoutMs:360000,needs:[[config.cargo,['--version']]],scope:'exact_synthetic_usb_writer_tests_no_device_or_gui'}),
    stage('serial_control_integrity',config.node,['scripts/test-background-controller-offline.mjs',join(output,'control-integrity'),'run','control-integrity'],
      {kind:'rust',selected:config.desktopCheck,timeoutMs:360000,needs:[[config.cargo,['--version']]],scope:'exact_raw_reader_control_and_legacy_tests_no_device_or_gui'}),
    stage('desktop_typecheck', config.cargo, ['check', '--tests', '--offline', '--locked',
      '--manifest-path', 'src-tauri/Cargo.toml'],
      {kind: 'check', selected: config.desktopCheck, needs: [[config.cargo, ['--version']]], timeoutMs: 1800000,
        scope: 'compile_only_no_bundle_validation', tauriConfigOverride: typeof process.env.TAURI_CONFIG === 'string'}),
  ];
}

export function counts(kind, output, artifact) {
  if (kind === 'check') return {passed: 1, skipped: 0, failed: 0};
  if (kind === 'browser') {
    const r = JSON.parse(artifact);
    if (!Number.isInteger(r.passed) || r.passed < 1 || r.results?.length !== r.passed
        || !r.results.every(x => x.passed === true) || r.error || r.pageErrors?.length
        || r.blockedExternalRequests?.length) throw Error('Browser report did not prove all cases passed');
    return {passed: r.passed, skipped: 0, failed: 0};
  }
  if (kind === 'pytest') {
    const suites = [...artifact.matchAll(/<testsuite\b([^>]*)>/g)];
    if (!suites.length) throw Error('Missing pytest JUnit suites');
    return suites.reduce((sum, [, attrs]) => {
      const value = key => {const m = attrs.match(new RegExp(`\\b${key}="(\\d+)"`)); if (!m) throw Error(`Missing JUnit ${key}`); return Number(m[1]);};
      const total = value('tests'), failed = value('failures') + value('errors'), skipped = value('skipped');
      if (failed + skipped > total) throw Error('Invalid JUnit counts');
      return {passed: sum.passed + total - failed - skipped, failed: sum.failed + failed, skipped: sum.skipped + skipped};
    }, {passed: 0, failed: 0, skipped: 0});
  }
  if (kind === 'rust') {
    const matches = [...output.matchAll(/test result: (?:ok|FAILED)\. (\d+) passed; (\d+) failed; (\d+) ignored;/g)];
    if (!matches.length) throw Error('Missing Rust test summary');
    return matches.reduce((sum, [, p, f, s]) => ({passed: sum.passed + Number(p),
      failed: sum.failed + Number(f), skipped: sum.skipped + Number(s)}), {passed: 0, failed: 0, skipped: 0});
  }
  const value = key => {
    const found = [...output.matchAll(new RegExp(`(?:^|\\n)(?:#|ℹ)\\s+${key}\\s+(\\d+)`, 'g'))];
    if (!found.length) throw Error(`Missing Node ${key} summary`);
    return Number(found.at(-1)[1]);
  };
  const passed = value('pass'), failed = value('fail') + value('cancelled'), skipped = value('skipped') + value('todo');
  if (passed + failed + skipped < value('tests')) throw Error('Incomplete Node test summary');
  return {passed, failed, skipped};
}

export async function execute(stages, runner, {failFast = false, source = () => ({valid: true, digest: 'synthetic'}), save = () => {}} = {}) {
  const readSource = async () => {try {return await source();} catch {return {valid: false, reason: 'source_snapshot_failed'};}};
  const before = await readSource();
  const report = {schema: 1, startedAt: new Date().toISOString(), source: before, stages: [],
    hardware: {status: 'pending', reason: 'Device access is prohibited in this runner'},
    acoustic: {status: 'pending', reason: 'Recording, playback and AEC acceptance are not run'},
    releaseReady: false, passed: false, status: 'running'};
  report.stages = stages.map(s => ({id: s.id, selected: s.selected,
    ...(s.scope ? {scope: s.scope, tauriConfigOverride: s.tauriConfigOverride} : {}),
    status: s.selected ? 'blocked' : 'skipped', reason: s.selected ? 'not_started' : 'not_selected'}));
  save(report);
  let stopped = false;
  for (const s of stages) {
    const entry = report.stages.find(x => x.id === s.id);
    delete entry.reason;
    if (!s.selected) Object.assign(entry, {status: 'skipped', reason: 'not_selected'});
    else if (!before.valid) entry.reason = 'source_version_unavailable';
    else if (stopped) entry.reason = 'fail_fast';
    else if (s.depends.some(id => report.stages.find(x => x.id === id)?.status !== 'passed')) entry.reason = 'dependency_not_passed';
    else {
      try {
        const ready = await runner.check(s);
        if (!ready.ok) entry.reason = ready.reason || 'missing_dependency';
        else {
          const result = await runner.run(s);
          entry.exitCode = result.code; entry.signal = result.signal || null;
          if (result.log) entry.log = result.log;
          if (result.blocked || result.error === 'ENOENT') entry.reason = result.blocked || 'missing_dependency';
          else if (result.code !== 0 || result.error || result.signal) Object.assign(entry, {status: 'failed', reason: result.error || 'nonzero_exit'});
          else {
            const resultCounts = result.counts || counts(s.kind, result.output, result.artifact);
            if (!['passed', 'failed', 'skipped'].every(k => Number.isSafeInteger(resultCounts[k]) && resultCounts[k] >= 0))
              throw Error('Invalid result counts');
            entry.tests = resultCounts;
            if (resultCounts.failed) Object.assign(entry, {status: 'failed', reason: 'failed_test_summary'});
            else if (resultCounts.skipped) Object.assign(entry, {status: 'skipped', reason: 'tests_skipped'});
            else if (!resultCounts.passed) entry.reason = 'no_tests_verified';
            else entry.status = 'passed';
          }
        }
      } catch (error) { entry.reason = 'verification_unavailable'; entry.diagnostic = String(error.message).slice(0, 300); }
    }
    if (s.selected && entry.status !== 'passed' && failFast) stopped = true;
    save(report);
  }
  const after = await readSource();
  report.sourceAfter = after;
  const consistent = before.valid && after.valid && before.digest === after.digest && before.commit === after.commit;
  report.stages.push({id: 'source_consistency', selected: true, status: consistent ? 'passed' : 'blocked',
    ...(!consistent ? {reason: 'source_changed_or_unavailable'} : {})});
  report.requestedStagesPassed = report.stages.filter(x => x.selected).every(x => x.status === 'passed');
  report.offlinePassed = report.stages.every(x => x.status === 'passed');
  report.status = report.stages.some(x => x.status === 'failed') ? 'failed'
    : report.stages.some(x => x.status === 'blocked') ? 'blocked' : report.offlinePassed ? 'offline_passed' : 'incomplete';
  report.exitCode = report.requestedStagesPassed ? 0 : report.status === 'failed' ? 1 : 2;
  report.finishedAt = new Date().toISOString(); save(report); return report;
}

/* All regular files in explicitly declared trees, including local BSP recipes,
 * test vendors and binary assets. Never follow source symlinks to private paths.
 * This bounds accidental reads and concurrent edits, not hostile-host attestation. */
export function snapshotInputs(root,trees,configurationFiles){
  const excluded=new Set(['__pycache__','node_modules','target','.pio','.pytest_cache','.git','.DS_Store']);
  const paths=new Map(),canonical=(a,b)=>a<b?-1:a>b?1:0;
  root=resolve(root);if(!lstatSync(root).isDirectory()||lstatSync(root).isSymbolicLink())throw Error('snapshot_root_invalid');
  const pathFor=rel=>{
    if(typeof rel!=='string'||isAbsolute(rel)||!rel||rel.includes('\0')||rel.split(/[\\/]/).some(p=>!p||p==='.'||p==='..'))throw Error('snapshot_path_invalid');
    let current=root;
    for(const part of rel.split(/[\\/]/)){current=join(current,part);if(lstatSync(current).isSymbolicLink())throw Error('snapshot_symlink');}
    return current;
  };
  const add=path=>{const rel=relative(root,path).split(sep).join('/');paths.set(rel,path);if(paths.size>8192)throw Error('snapshot_file_limit');};
  const walk=(directory,depth=0)=>{
    if(depth>32)throw Error('snapshot_depth_limit');
    for(const entry of readdirSync(directory,{withFileTypes:true})){
      if(excluded.has(entry.name))continue;
      const path=join(directory,entry.name);
      if(entry.isSymbolicLink())throw Error('snapshot_symlink');
      if(entry.isDirectory())walk(path,depth+1);
      else if(entry.isFile())add(path);
      else throw Error('snapshot_file_type');
    }
  };
  for(const path of trees)walk(pathFor(path));
  for(const path of configurationFiles)add(pathFor(path));
  const hash=createHash('sha256');let bytes=0;
  for(const [name,path]of [...paths].sort(([a],[b])=>canonical(a,b))){
    const fd=openSync(path,constants.O_RDONLY|(constants.O_NOFOLLOW||0));
    try{
      const before=fstatSync(fd,{bigint:true});
      if(!before.isFile()||before.size>32n*1024n*1024n||before.size<0n)throw Error('snapshot_file_size');
      bytes+=Number(before.size);if(bytes>128*1024*1024)throw Error('snapshot_byte_limit');
      const content=Buffer.alloc(Number(before.size));let offset=0;
      while(offset<content.length){const n=readSync(fd,content,offset,content.length-offset,null);if(!n)throw Error('snapshot_file_changed');offset+=n;}
      const extra=Buffer.alloc(1),after=fstatSync(fd,{bigint:true});
      if(readSync(fd,extra,0,1,null)||before.size!==after.size||before.mtimeNs!==after.mtimeNs||before.ctimeNs!==after.ctimeNs)throw Error('snapshot_file_changed');
      const pathBytes=Buffer.from(name,'utf8'),lengths=Buffer.alloc(12);
      lengths.writeUInt32LE(pathBytes.length);lengths.writeBigUInt64LE(before.size,4);
      hash.update(lengths).update(pathBytes).update(content);
    }finally{closeSync(fd);}
  }
  return {digest:hash.digest('hex'),files:paths.size,bytes};
}
export function sourceVersion(config) {
  const git = args => spawnSync(config.git, args, {cwd: repoRoot, encoding: 'utf8', timeout: 10000});
  const commit = git(['rev-parse', 'HEAD']), status = git(['status', '--porcelain']);
  const configurationFiles = [...new Set([...SNAPSHOT_FILES,
    ...readdirSync(join(pcRoot, 'src-tauri')).filter(name => /^tauri(?:\.[^.]+)*\.conf\.json$/.test(name))
      .map(name => `pc/src-tauri/${name}`)])].sort();
  const snapshot=snapshotInputs(repoRoot,SNAPSHOT_TREES,configurationFiles);
  const pkg = JSON.parse(readFileSync(join(pcRoot, 'package.json'), 'utf8'));
  return {valid: commit.status === 0 && status.status === 0, commit: commit.stdout?.trim() || null,
    dirty: !!status.stdout?.trim(), clientVersion: pkg.version,...snapshot,
    scope: {trees: SNAPSHOT_TREES, configurationFiles,regularFiles:'all_except_declared_caches',symlinks:'rejected',hashEncoding:'length-framed-path-bytes-v2',entireRepository: false}};
}

function processRun(command, args, settings = {}) {
  return new Promise(resolveResult => {
    let output = '', error = null, limit = false;
    const child = spawn(command, args, {cwd: settings.cwd, env: settings.env, stdio: ['ignore', 'pipe', 'pipe'], shell: false});
    const receive = data => {if (output.length < 16 * 1024 * 1024) output += data.toString(); else {limit = true; child.kill('SIGTERM');}};
    child.stdout.on('data', receive); child.stderr.on('data', receive);
    child.on('error', e => {error = e.code || 'spawn_error';});
    let killer;
    const timer = setTimeout(() => {error = 'timeout'; child.kill('SIGTERM'); killer = setTimeout(() => child.kill('SIGKILL'), 2000);}, settings.timeoutMs || 15000);
    child.on('close', (code, signal) => {clearTimeout(timer); clearTimeout(killer); resolveResult({code, signal, output, error: limit ? 'log_limit' : error});});
  });
}

function realRunner(config, output) {
  const paths = [config.node, config.cargo].filter(isAbsolute).map(dirname);
  const env = {...process.env, PATH: [...paths, process.env.PATH || ''].join(delimiter),
    CARGO_NET_OFFLINE: 'true', npm_config_offline: 'true', npm_config_audit: 'false', npm_config_fund: 'false',
    npm_config_update_notifier: 'false', PYTEST_DISABLE_PLUGIN_AUTOLOAD: '1', PYTHONDONTWRITEBYTECODE: '1', PIP_NO_INDEX: '1',
    CARGO_PROFILE_DEV_DEBUG: '0', CARGO_PROFILE_DEV_INCREMENTAL: 'false',
    CARGO_TARGET_DIR: config.targetDir || join(output, 'rust-target'),
    PET_WORKFLOW_CARGO: config.cargo, PET_GRAPHICS_CARGO: config.cargo,PET_BACKGROUND_CARGO:config.cargo,
    PET_BACKGROUND_TARGET_DIR:process.env.PET_BACKGROUND_TARGET_DIR||config.targetDir||join(output,'rust-target'),
    PET_WORKFLOW_TARGET_DIR: config.targetDir || join(output, 'rust-target'),
    PET_GRAPHICS_TARGET_DIR: config.targetDir || join(output, 'rust-target')};
  if (config.browserExecutable) env.PET_TEST_BROWSER = config.browserExecutable;
  env.PET_FRAMEWORK_UI_URL = `http://127.0.0.1:${config.port}/tests/manual/framework-ui.html`;
  env.PET_BACKGROUND_UI_URL = `http://127.0.0.1:${config.port}/tests/manual/program-background.html`;
  return {
    async check(stage) {
      for (const path of stage.requiredFiles || []) {
        try {if (!statSync(join(repoRoot, path)).isFile()) throw Error();} catch {return {ok: false, reason: `missing_dependency:${path}`};}
      }
      for (const [command, args] of stage.needs || []) {
        const r = await processRun(command, args, {cwd: stage.cwd, env});
        if (r.code !== 0) return {ok: false, reason: `missing_dependency:${command}`};
      }
      return {ok: true};
    },
    async run(stage) {
      let server, timer, killTimer, serverOutput = '';
      try {
        if (stage.kind === 'browser') {
          server = spawn(config.node, ['node_modules/vite/bin/vite.js', '--host', '127.0.0.1', '--port', String(config.port), '--strictPort'],
            {cwd: pcRoot, env, shell: false, stdio: ['ignore', 'pipe', 'pipe']});
          await new Promise((ready, failed) => {
            timer = setTimeout(() => failed(Error('Loopback Vite did not become ready')), 15000);
            const receive = data => {serverOutput += data.toString(); if (serverOutput.includes(`127.0.0.1:${config.port}`)) ready();};
            server.stdout.on('data', receive); server.stderr.on('data', receive);
            server.once('error', failed); server.once('exit', code => failed(Error(`Vite exited: ${code}`)));
          });
          clearTimeout(timer);
        }
        const r = await processRun(stage.command, stage.args, {cwd: stage.cwd, env, timeoutMs: stage.timeoutMs});
        const log = join(output, `${stage.id}.log`); writeFileSync(log, r.output);
        const artifact = stage.junit || stage.browserReport;
        const blocked = /no matching package named|failed to download|attempting to make an HTTP request.*offline|Executable doesn't exist/i.test(r.output)
          ? 'missing_offline_dependency' : null;
        return {...r, log: relative(output, log), blocked,
          artifact: r.code === 0 && artifact ? readFileSync(artifact, 'utf8') : undefined};
      } finally {
        clearTimeout(timer);
        if (server) {
          server.kill('SIGTERM');
          if (server.exitCode === null) await new Promise(done => {
            server.once('close', done); killTimer = setTimeout(() => {server.kill('SIGKILL'); done();}, 2000);
          });
          clearTimeout(killTimer); writeFileSync(join(output, 'browser-server.log'), serverOutput);
        }
      }
    },
  };
}

export async function main(argv = process.argv.slice(2)) {
  const config = options(argv);
  mkdirSync(config.output, {recursive: true});
  const output = mkdtempSync(join(config.output, 'framework-offline-'));
  const save = report => writeFileSync(join(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  const report = await execute(plan(config, output), realRunner(config, output),
    {failFast: config.failFast, source: () => sourceVersion(config), save});
  console.log(JSON.stringify({report: join(output, 'report.json'), status: report.status,
    requestedStagesPassed: report.requestedStagesPassed, hardware: 'pending', acoustic: 'pending', exitCode: report.exitCode}));
  return report.exitCode;
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().then(code => {process.exitCode = code;}).catch(error => {console.error(error.message); process.exitCode = 2;});
}
