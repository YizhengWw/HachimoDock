import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createRequire} from 'node:module';
import {runInNewContext} from 'node:vm';
import * as backgroundModule from './program-background.js';
import * as autostartModule from './program-autostart.js';
import {AUTOSTART_IDLE, PROGRAM_AUTOSTART_NOTICE, autostartTarget, autostartStatus, createProgramAutostartController, supportsProgramAutostart, programAutostartMessage} from './program-autostart.js';

const target = {boardId: 'p4-test', componentId: 'focus', contentHash: 'a'.repeat(64),
  checksums: {widgetChecksum: 'ffffffff', buttonsChecksum: '00000001', spritesChecksum: '00000000'}};
const bootId = '0123456789abcdef';
const status = (policy = null, code = policy ? 'enabled' : 'disabled', attempts = 0) => ({schemaVersion: 1, enabled: policy !== null, policy, attempts, code});
function deferred() {let resolve, reject; const promise = new Promise((a, b) => {resolve = a; reject = b;}); return {promise, resolve, reject};}

test('autostart accepts real v2 negotiation and preserves the one shared PROGRAM lane', () => {
  const caps = {widgetProgram: 'p4-component-program-v2', widgetPrograms: ['p4-component-program-v1', 'p4-component-program-v2'],
    widgetProgramBackground: 'p4-program-background-v2', widgetProgramInspect: 'p4-program-inspect-v1', widgetProgramBackgroundMaxActive: 1,
    widgetProgramAggregateFuel: 4096, widgetProgramMaxFuel: 4096, widgetProgramQuantum: 64, widgetProgramAggregateQuantum: 64};
  assert.equal(supportsProgramAutostart(caps), true);
  for (const change of [{widgetProgram: 'p4-component-program-v3'}, {widgetProgram: caps.widgetPrograms}, {widgetProgramBackgroundMaxActive: 2},
    {widgetProgramBackgroundMaxActive: 4}, {widgetPrograms: ['p4-component-program-v1']}, {widgetProgramInspect: null}]) {
    assert.equal(supportsProgramAutostart({...caps, ...change}), false);
  }
  assert.equal(supportsProgramAutostart(), false); assert.equal(supportsProgramAutostart(null), false);
});

test('autostart requires all four exact public budgets; manual background compatibility stays unchanged', () => {
  const caps = {widgetProgram: 'p4-component-program-v2', widgetPrograms: ['p4-component-program-v1', 'p4-component-program-v2'],
    widgetProgramBackground: 'p4-program-background-v2', widgetProgramInspect: 'p4-program-inspect-v1', widgetProgramBackgroundMaxActive: 1,
    widgetProgramAggregateFuel: 4096, widgetProgramMaxFuel: 4096, widgetProgramQuantum: 64, widgetProgramAggregateQuantum: 64};
  assert.equal(supportsProgramAutostart(caps), true);
  for (const [key, expected] of [['widgetProgramAggregateFuel', 4096], ['widgetProgramMaxFuel', 4096], ['widgetProgramQuantum', 64], ['widgetProgramAggregateQuantum', 64]]) {
    const missing = {...caps}; delete missing[key];
    assert.equal(supportsProgramAutostart(missing), false, `missing ${key}`);
    assert.equal(backgroundModule.supportsBackgroundPrograms(missing), true, `manual missing ${key}`);
    for (const value of [undefined, null, true, String(expected), 0, -1, expected - 1, expected + 1, expected * 2, 1.5, [], {}]) {
      const invalid = {...caps, [key]: value};
      assert.equal(supportsProgramAutostart(invalid), false, `${key}: ${JSON.stringify(value)}`);
      assert.equal(backgroundModule.supportsBackgroundPrograms(invalid), true, `manual ${key}: ${JSON.stringify(value)}`);
    }
  }
  const legacy = {widgetProgram: 'p4-component-program-v1', widgetProgramBackground: 'p4-program-background-v2',
    widgetProgramInspect: 'p4-program-inspect-v1', widgetProgramBackgroundMaxActive: 1};
  assert.equal(backgroundModule.supportsBackgroundPrograms(legacy), true);
  assert.equal(supportsProgramAutostart(legacy), false);
});

test('policy describes PC-online behavior and creation/refresh never submits USB starts', async () => {
  const calls = [], c = createProgramAutostartController({invoke: (name, args) => {calls.push([name, args]); return status();}});
  assert.equal(c.getState(), AUTOSTART_IDLE); assert.equal(calls.length, 0);
  await c.refresh(); await c.refresh();
  assert.deepEqual(calls.map(([name]) => name), ['program_autostart_get', 'program_autostart_get']);
  assert.match(PROGRAM_AUTOSTART_NOTICE, /PC 在线托管/); assert.match(PROGRAM_AUTOSTART_NOTICE, /断开 PC/);
});

test('exact board/content/checksum identity is validated and foreign data is discarded', () => {
  const clean = autostartTarget({...target, privatePath: 'secret', checksums: {...target.checksums, apiKey: 'secret'}});
  assert.deepEqual(clean, target); assert.ok(Object.isFrozen(clean)); assert.ok(Object.isFrozen(clean.checksums));
  assert.equal(JSON.stringify(clean).includes('secret'), false);
  for (const change of [{boardId: ''}, {boardId: ' test '}, {componentId: '../focus'}, {componentId: 'Focus'},
    {contentHash: 'a'.repeat(16)}, {contentHash: 'A'.repeat(64)}, {checksums: {...target.checksums, widgetChecksum: '0'}}, {checksums: null}]) {
    assert.throws(() => autostartTarget({...target, ...change}), /invalid_autostart_target/);
  }
});

test('status accepts only bounded native policy data', () => {
  assert.equal(autostartStatus(status(target, 'result_unknown_no_replay', 256)).attempts, 256);
  for (const change of [{schemaVersion: 2}, {enabled: true}, {policy: undefined}, {code: 'private-key'}, {attempts: -1}, {attempts: 257}, {attempts: 1.5}]) {
    assert.throws(() => autostartStatus({...status(), ...change}), /invalid_autostart_status/);
  }
  const clean = autostartStatus({...status(target), debug: 'secret'});
  assert.equal(JSON.stringify(clean).includes('secret'), false);
});

test('only explicit true consent enables, and enable itself never observes or starts', async () => {
  const calls = [], c = createProgramAutostartController({invoke: (name, args) => {calls.push([name, args]); return status(target);}});
  for (const userConfirmed of [undefined, false, 1, 'true']) assert.throws(() => c.enable(target, {userConfirmed}), /explicit_consent/);
  assert.equal(calls.length, 0);
  await c.enable({...target, privatePath: 'secret'}, {userConfirmed: true});
  assert.deepEqual(calls, [['program_autostart_enable', {target, userConfirmed: true}]]);
});

test('prepare is a read-only native preview; changed consent content stays exact in enable payload', async () => {
  const calls = [], c = createProgramAutostartController({invoke: (name, args) => {
    calls.push([name, args]); return name === 'program_autostart_inspect' ? target : status(target);
  }});
  const preview = await c.prepare({boardId: target.boardId, componentId: target.componentId});
  assert.deepEqual(preview, target); assert.equal(c.getState().policy, null);
  assert.equal(calls.length, 1); assert.equal(calls[0][0], 'program_autostart_inspect');
  await c.enable(preview, {userConfirmed: true}); assert.equal(calls[1][1].target.contentHash, preview.contentHash);
});

test('wrong-board/invalid/rejected preview cannot become a consent target or leak private errors', async () => {
  for (const reply of [{...target, boardId: 'other'}, {...target, componentId: 'other'}, {...target, contentHash: 'short'}, Error('secret-path')]) {
    const c = createProgramAutostartController({invoke: () => {if (reply instanceof Error) throw reply; return reply;}});
    await assert.rejects(c.prepare({boardId: target.boardId, componentId: target.componentId}), /verification_unavailable/);
    assert.equal(c.getState().policy, null); assert.equal(JSON.stringify(c.getState()).includes('secret'), false);
  }
});

test('native observation is explicit and uses exact board plus boot without any start IPC', async () => {
  const calls = [], c = createProgramAutostartController({invoke: (name, args) => {calls.push([name, args]); return status(target, 'running', 1);}});
  await c.connected({boardId: target.boardId, bootId});
  assert.deepEqual(calls, [['program_autostart_observe', {boardId: target.boardId, bootId}]]);
  for (const invalid of ['0123456789abcdeF', '', 123]) assert.throws(() => c.connected({boardId: target.boardId, bootId: invalid}), /invalid_autostart_connection/);
});

test('one flight rejects overlapping enable/disable/observe/read and never queues a later start', async () => {
  const wait = deferred(), calls = [], c = createProgramAutostartController({invoke: (name, args) => {calls.push([name, args]); return wait.promise;}});
  const active = c.connected({boardId: target.boardId, bootId});
  await assert.rejects(c.disable(), /busy/); await assert.rejects(c.refresh(), /busy/);
  await assert.rejects(c.enable(target, {userConfirmed: true}), /busy/);
  await assert.rejects(c.prepare({boardId: target.boardId, componentId: target.componentId}), /busy/);
  wait.resolve(status(target, 'result_unknown_no_replay', 1)); await active;
  assert.equal(calls.length, 1); assert.equal(c.getState().code, 'result_unknown_no_replay');
});

test('lost/invalid IPC outcome latches unknown without automatic replay; refresh is read-only', async () => {
  for (const reply of [undefined, {confirmed: true}, {...status(target), attempts: 999}, Error('private-token')]) {
    const calls = [], c = createProgramAutostartController({invoke: name => {
      calls.push(name); if (name === 'program_autostart_get') return status(target, 'result_unknown_no_replay', 1);
      if (reply instanceof Error) throw reply; return reply;
    }});
    await c.connected({boardId: target.boardId, bootId});
    assert.equal(c.getState().status, 'unknown'); assert.equal(c.getState().code, 'result_unknown_no_replay');
    assert.equal(JSON.stringify(c.getState()).includes('private-token'), false); assert.equal(calls.length, 1);
    await c.refresh(); assert.deepEqual(calls, ['program_autostart_observe', 'program_autostart_get']);
    assert.equal(c.getState().code, 'result_unknown_no_replay');
  }
});

test('duplicate connections and controller remount retain the native durable latch', async () => {
  // Native command fake returns its durable result; neither client may issue a
  // legacy USB start command to compensate for an already-attempted boot.
  const calls = [], invoke = name => {calls.push(name); return status(target, 'result_unknown_no_replay', 1);};
  const a = createProgramAutostartController({invoke});
  await a.connected({boardId: target.boardId, bootId}); await a.connected({boardId: target.boardId, bootId}); a.dispose();
  const b = createProgramAutostartController({invoke}); await b.refresh(); await b.connected({boardId: target.boardId, bootId});
  assert.deepEqual(calls, ['program_autostart_observe', 'program_autostart_observe', 'program_autostart_get', 'program_autostart_observe']);
  assert.equal(b.getState().attempts, 1); assert.equal(b.getState().code, 'result_unknown_no_replay');
});

test('disable only revokes policy and does not silently stop a replacement registration', async () => {
  const calls = [], c = createProgramAutostartController({invoke: name => {calls.push(name); return status(null, 'disabled', 1);}});
  await c.disable(); assert.equal(c.getState().policy, null); assert.equal(c.getState().attempts, 1);
  assert.deepEqual(calls, ['program_autostart_disable']);
});

test('revocations and preemption status never schedule another request', async () => {
  for (const code of ['revoked_content_changed', 'revoked_component_deleted', 'revoked_device_changed', 'preempted_no_resume', 'owner_present_no_resume', 'journal_full', 'storage_unavailable']) {
    const calls = [], policy = code === 'preempted_no_resume' || code === 'owner_present_no_resume' ? target : null;
    const c = createProgramAutostartController({invoke: name => {calls.push(name); return status(policy, code, 1);}});
    await c.refresh(); assert.equal(c.getState().code, code); assert.deepEqual(calls, ['program_autostart_get']);
  }
});

test('dispose does not publish late replies or allow a prepared stale target to escape', async () => {
  const wait = deferred(), views = [], c = createProgramAutostartController({invoke: () => wait.promise, onChange: s => views.push(s)});
  const read = c.refresh(); c.dispose(); const count = views.length;
  wait.resolve(status(target)); await read; assert.equal(views.length, count);
  await assert.rejects(c.refresh(), /disposed/); await assert.rejects(c.disable(), /disposed/);
  const preview = deferred(), d = createProgramAutostartController({invoke: () => preview.promise});
  const prepared = d.prepare({boardId: target.boardId, componentId: target.componentId}); d.dispose(); preview.resolve(target);
  await assert.rejects(prepared, /verification_unavailable/);
});

test('a failing view callback cannot leave the single-flight flag stuck', async () => {
  let fail = true, calls = 0;
  const c = createProgramAutostartController({invoke: () => {calls++; return status();}, onChange: () => {if (fail) throw Error('view-failed');}});
  await assert.rejects(c.refresh(), /view-failed/); fail = false;
  await c.refresh(); assert.equal(calls, 1); assert.equal(c.getState().pending, false);
});

test('explicit stop suppression is a boot-specific policy command and never a synthetic registration stop', async () => {
  const calls = [], c = createProgramAutostartController({invoke: (name, args) => {calls.push([name, args]); return status(null, 'preempted_no_resume', 1);}});
  await c.suppress({boardId: target.boardId, bootId});
  assert.deepEqual(calls, [['program_autostart_suppress_boot', {boardId: target.boardId, bootId}]]);
  assert.equal(c.getState().code, 'preempted_no_resume');
  assert.throws(() => c.suppress({boardId: '', bootId}), /invalid_autostart_connection/);
});

test('status copy preserves unknown/preemption meaning and never claims disconnected-device boot startup', () => {
  assert.match(programAutostartMessage(AUTOSTART_IDLE), /读取/);
  assert.match(programAutostartMessage(autostartStatus(status(target, 'result_unknown_no_replay', 1))), /未知.*不会自动重发/);
  assert.match(programAutostartMessage(autostartStatus(status(target, 'preempted_no_resume', 1))), /不会自动恢复/);
  assert.match(programAutostartMessage(autostartStatus(status(target, 'running', 1))), /当前运行状态请查看/);
  assert.match(programAutostartMessage(autostartStatus(status())), /不会立即启动/);
});
test('optional native policy unavailability blocks only autostart and is not treated as pending or a retry trigger', async () => {
  for (const error of ['program_autostart_unavailable', Error('program_autostart_unavailable')]) {
    const calls = [], c = createProgramAutostartController({invoke: name => {calls.push(name); throw error;}});
    await c.refresh(); assert.equal(c.getState().status, 'unavailable'); assert.equal(c.getState().pending, false);
    assert.equal(c.getState().policy, null); assert.equal(c.getState().code, 'storage_unavailable');
    assert.match(programAutostartMessage(c.getState()), /自动启动已阻止/); assert.deepEqual(calls, ['program_autostart_get']);
  }
});

test('v1/v2 entry and explicit-consent UI render in Node without GUI, USB or mount-time execution', () => {
  const require = createRequire(import.meta.url), React = require('react');
  const {renderToStaticMarkup} = require('react-dom/server'), {transformSync} = require('esbuild');
  // In-memory JSX transpile + SSR only: no bundling, files, DOM or browser.
  const {code} = transformSync(readFileSync(new URL('./ProgramBackground.jsx', import.meta.url), 'utf8'), {loader: 'jsx', format: 'cjs', target: 'es2022'});
  const module = {exports: {}};
  runInNewContext(code, {module, exports: module.exports, require: id => {
    if (id === 'react') return React;
    if (id === '@tauri-apps/api/core') return {invoke: () => assert.fail('unexpected native execution')};
    if (id === './program-background.js') return backgroundModule;
    if (id === './program-autostart.js') return autostartModule;
    if (id === './program-background.css') return {};
    if (id === '../shell/Button') return {default: ({children, disabled, onClick}) => React.createElement('button', {disabled, onClick}, children), __esModule: true};
    assert.fail(`unexpected import: ${id}`);
  }});
  const {ProgramBackgroundProvider, ProgramBackgroundStart, ProgramBackgroundStatus} = module.exports;
  const caps = {widgetProgram: 'p4-component-program-v2', widgetPrograms: ['p4-component-program-v1', 'p4-component-program-v2'],
    widgetProgramBackground: 'p4-program-background-v2', widgetProgramInspect: 'p4-program-inspect-v1', widgetProgramBackgroundMaxActive: 1,
    widgetProgramAggregateFuel: 4096, widgetProgramMaxFuel: 4096, widgetProgramQuantum: 64, widgetProgramAggregateQuantum: 64};
  const render = (engine, capabilities = caps) => renderToStaticMarkup(React.createElement(ProgramBackgroundProvider,
    {usb: {connected: true, boardDeviceId: target.boardId, runtime: 'esp-p4', capabilities}, request: () => assert.fail('SSR must not invoke')},
    React.createElement(React.Fragment, null, React.createElement(ProgramBackgroundStart,
      {component: {id: 'focus', appRuntime: {program: {engine}}}, isInstalled: true}), React.createElement(ProgramBackgroundStatus))));
  for (const engine of caps.widgetPrograms) {
    const html = render(engine); assert.match(html, /在设备后台运行/); assert.match(html, /核对授权内容/);
    assert.match(html, /PC 在线托管/); assert.match(html, /停用连接后启动/);
    // Consent target must be fetched and checked before the enable action exists.
    assert.equal(html.includes('明确启用连接后启动'), false);
  }
  assert.equal(render('p4-component-program-v3').includes('在设备后台运行'), false);
  const legacy = {...caps}; delete legacy.widgetProgramAggregateFuel;
  const manualOnly = render('p4-component-program-v2', legacy);
  assert.match(manualOnly, /在设备后台运行/); assert.equal(manualOnly.includes('核对授权内容'), false);
  assert.equal(render('p4-component-program-v2', {...caps, widgetProgramInspect: null}), '');
});
