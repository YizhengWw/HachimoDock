// Development-only harness. No real USB, credentials, model calls or filesystem writes.
import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import App from '../src/App.jsx';
import PersonaVoiceModal from '../src/PersonaVoiceModal.jsx';
import { normalizePersonaVoice } from '../src/lib/persona-voice.js';
import '../src/styles.css';

let failStorage = false, discard = false;
const originalSetItem = Storage.prototype.setItem;
Storage.prototype.setItem = function(key, value) {
  if (failStorage && key.startsWith('claw-pet.video-gen-config.')) throw Error('模拟磁盘满');
  return originalSetItem.call(this, key, value);
};
window.__TAURI_EVENT_PLUGIN_INTERNALS__ = { unregisterListener() {} };
window.__TAURI_INTERNALS__ = {
  transformCallback: () => 1, unregisterCallback() {}, convertFileSrc: x => x,
  async invoke(command, args = {}) {
    switch (command) {
      case 'load_generation_provider_configs': return {};
      case 'save_generation_provider_config': if(failStorage)throw Error('模拟写入失败，配置未保存');return;
      case 'set_exit_reasons': return;
      case 'load_device_bindings': return [];
      case 'usb_get_status': return { connected: false };
      case 'usb_scan_devices': return [];
      case 'load_realtime_voice_volume': return 0.3;
      case 'load_voice_chat_settings': throw Error('模拟 LLM 配置读取失败');
      case 'load_device_asr_settings': return { configured: true, message: '语音配置独立读取成功（模拟）' };
      case 'realtime_chat_status': return { sessionId: '', active: false };
      case 'plugin:event|listen': return 1;
      case 'plugin:event|unlisten': return;
      case 'plugin:dialog|message': return discard ? args.buttons.ok : args.buttons.cancel;
      case 'list_component_library': return { components: [], libraryPath: '' };
      case 'community_request': return { owned: {}, downloaded: {}, reservedIds: [], items: [], total: 0 };
      default: throw Error(`隔离环境未实现：${command}`);
    }
  },
};
function Harness() {
  const [modal, setModal] = useState(false), [failure, setFailure] = useState(false), [allow, setAllow] = useState(false);
  return <><aside style={{ padding: 8, background: '#fff0de' }}>
    <strong>隔离测试 · 不读写用户配置或设备</strong>{' '}
    <button onClick={() => { failStorage = !failure; setFailure(!failure); }}>存储故障：{failure ? '开启' : '关闭'}</button>{' '}
    <button onClick={() => { discard = !allow; setAllow(!allow); }}>放弃确认：{allow ? '允许' : '取消'}</button>{' '}
    <button onClick={() => setModal(true)}>测试人设弹窗</button>
  </aside><App />{modal && <PersonaVoiceModal appearance={{ id: 'synthetic', name: '测试小狗', type: 'custom',
    personaVoice: normalizePersonaVoice(null, { name: '测试小狗' }) }} previewAvailable={false}
    onClose={() => setModal(false)} />}</>;
}
createRoot(document.getElementById('root')).render(<Harness />);
