// PC-online policy UI only. Native Rust owns authorization, disk journal and USB.
// Reading status never starts a program; there is no timer, retry or RAM resume.
import {supportsBackgroundPrograms} from './program-background.js';
const boardValid = s => typeof s === 'string' && /^[A-Za-z0-9._-]{1,47}$/.test(s);
const componentValid = s => typeof s === 'string' && /^[a-z][a-z0-9_-]{0,46}$/.test(s);
const hashValid = s => typeof s === 'string' && /^[0-9a-f]{64}$/.test(s);
const bootValid = s => typeof s === 'string' && /^[0-9a-f]{16}$/.test(s);
const checksumValid = s => typeof s === 'string' && /^[0-9a-f]{8}$/.test(s);
const unavailable = error => error === 'program_autostart_unavailable' || (error instanceof Error && error.message === 'program_autostart_unavailable');
const codes = new Set(['disabled', 'enabled', 'running', 'result_unknown_no_replay',
  'already_attempted', 'owner_present_no_resume', 'preempted_no_resume',
  'revoked_content_changed', 'revoked_component_deleted', 'revoked_device_changed',
  'verification_unavailable', 'boot_changed', 'journal_full', 'storage_unavailable']);

export const PROGRAM_AUTOSTART_NOTICE = '连接后启动由此 PC 在线托管。仅对您授权的设备和组件内容生效，每次设备开机最多尝试一次；设备断开 PC 后不会因此自行启动。音频抢占或结果未知后不会自动恢复或重发。';
export const AUTOSTART_IDLE = Object.freeze({status: 'idle', pending: false, policy: null, attempts: 0, code: 'disabled'});

export function supportsProgramAutostart(caps = {}) {
  return supportsBackgroundPrograms(caps) && caps.widgetProgramBackgroundMaxActive === 1
    && caps.widgetProgramAggregateFuel === 4096 && caps.widgetProgramMaxFuel === 4096
    && caps.widgetProgramQuantum === 64 && caps.widgetProgramAggregateQuantum === 64;
}
export function programAutostartMessage(state) {
  if (state.pending) return '正在核对或保存连接后启动策略；不会重复提交。';
  if (state.status === 'idle') return '正在读取连接后启动策略…';
  if (state.status === 'unknown') return '上次结果未确认，不会自动重发。请刷新核对策略；停用仍可显式提交。';
  const messages = {
    disabled: '连接后启动未启用。启用只保存授权，不会立即启动程序。',
    enabled: '已保存固定设备和内容的授权；下次连接或设备开机后最多尝试一次。',
    running: '本次开机已确认启动；当前运行状态请查看设备后台程序。',
    result_unknown_no_replay: '本次开机的启动结果未知，已锁存，不会自动重发。',
    already_attempted: '本次开机已有尝试记录，不会再次自动启动。',
    owner_present_no_resume: '本次开机已有程序注册，不会自动替换或恢复。',
    preempted_no_resume: '本次开机已主动停止或被音频抢占，不会自动恢复。',
    revoked_content_changed: '组件内容已变化，授权已撤销。请重新核对并明确授权。',
    revoked_component_deleted: '组件已删除，授权已撤销。',
    revoked_device_changed: '连接设备已变化，原设备授权已撤销。',
    verification_unavailable: '本次开机未能完成内容核验，未自动启动，也不会自动重试。',
    boot_changed: '核验期间设备已重新开机，本次记录不会重试。',
    journal_full: '启动记录已满，已停止自动启动；仍可显式手动启动。',
    storage_unavailable: '本地启动记录不可用，自动启动已阻止。',
  };
  return messages[state.code] || '正在读取连接后启动策略…';
}

export function autostartTarget(value) {
  if (!value || !boardValid(value.boardId) || !componentValid(value.componentId) || !hashValid(value.contentHash)
    || !value.checksums || !['widgetChecksum', 'buttonsChecksum', 'spritesChecksum'].every(k => checksumValid(value.checksums[k]))) {
    throw Error('invalid_autostart_target');
  }
  return Object.freeze({boardId: value.boardId, componentId: value.componentId, contentHash: value.contentHash,
    checksums: Object.freeze(Object.fromEntries(['widgetChecksum', 'buttonsChecksum', 'spritesChecksum'].map(k => [k, value.checksums[k]])))});
}

export function autostartStatus(value) {
  if (!value || value.schemaVersion !== 1 || typeof value.enabled !== 'boolean' || !codes.has(value.code)
    || !Number.isInteger(value.attempts) || value.attempts < 0 || value.attempts > 256
    || (value.policy !== null && !value.policy) || value.enabled !== (value.policy !== null)) throw Error('invalid_autostart_status');
  return Object.freeze({status: 'ready', pending: false, policy: value.policy === null ? null : autostartTarget(value.policy),
    attempts: value.attempts, code: value.code});
}

export function createProgramAutostartController({invoke, onChange = () => {}}) {
  if (typeof invoke !== 'function') throw Error('invalid_autostart_transport');
  let alive = true, pending = false, state = AUTOSTART_IDLE;
  const publish = next => {if (alive) {state = Object.freeze(next); onChange(state);} return state;};
  async function request(command, args = {}) {
    if (!alive) throw Error('autostart_disposed');
    if (pending) throw Error('autostart_busy');
    pending = true;
    try {
      publish({...state, pending: true});
      const next = autostartStatus(await invoke(command, args));
      return publish(next);
    } catch (error) {
      // Includes IPC rejection: enabling/disabling/observing may already have
      // reached native. A refresh may read the latch; it cannot replay a start.
      return publish(unavailable(error) ? {...state, status: 'unavailable', policy: null, pending: false, code: 'storage_unavailable'}
        : {...state, status: 'unknown', pending: false, code: 'result_unknown_no_replay'});
    } finally {pending = false;}
  }
  return Object.freeze({
    getState: () => state,
    refresh: () => request('program_autostart_get'),
    async prepare({boardId, componentId}) {
      if (!alive) throw Error('autostart_disposed');
      if (pending) throw Error('autostart_busy');
      if (!boardValid(boardId) || !componentValid(componentId)) throw Error('invalid_autostart_target');
      pending = true;
      try {
        publish({...state, pending: true});
        const target = autostartTarget(await invoke('program_autostart_inspect', {boardId, componentId}));
        if (target.boardId !== boardId || target.componentId !== componentId) throw Error('autostart_owner_changed');
        if (!alive) throw Error('autostart_disposed');
        publish({...state, pending: false});
        return target;
      } catch (error) {
        publish(unavailable(error) ? {...state, status: 'unavailable', policy: null, pending: false, code: 'storage_unavailable'}
          : {...state, status: 'unknown', pending: false, code: 'verification_unavailable'});
        throw Error('autostart_verification_unavailable');
      } finally {pending = false;}
    },
    enable(target, {userConfirmed = false} = {}) {
      if (userConfirmed !== true) throw Error('autostart_explicit_consent_required');
      return request('program_autostart_enable', {target: autostartTarget(target), userConfirmed: true});
    },
    disable: () => request('program_autostart_disable'),
    // One application-level connection owner, after authoritative boot query.
    // Reconnect duplicates are harmless: the durable native ledger is final.
    connected({boardId, bootId}) {
      if (!boardValid(boardId) || !bootValid(bootId)) throw Error('invalid_autostart_connection');
      return request('program_autostart_observe', {boardId, bootId});
    },
    suppress({boardId, bootId}) {
      if (!boardValid(boardId) || !bootValid(bootId)) throw Error('invalid_autostart_connection');
      return request('program_autostart_suppress_boot', {boardId, bootId});
    },
    dispose() {alive = false;},
  });
}
