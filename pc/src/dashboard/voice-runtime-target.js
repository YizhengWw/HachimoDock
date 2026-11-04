// Runtime identity must come from the completed USB handshake, never a fallback.
export function voiceRuntimeTarget(usb, agentId = '') {
  const runtime = String(usb?.runtime || '').trim().toLowerCase();
  const known = runtime === 'esp-p4' || ['linux','linux-board','raspberry-pi','raspberry','radxa'].includes(runtime);
  const connectionId = typeof usb?.connectionId === 'string'
    && /^[1-9][0-9]{0,19}$/.test(usb.connectionId)
    && BigInt(usb.connectionId) <= 18446744073709551615n ? usb.connectionId : '';
  const bootId = typeof usb?.bootId === 'string' && /^[0-9a-f]{16}$/.test(usb.bootId) ? usb.bootId : '';
  const ready = usb?.connected === true && known && Boolean(usb?.boardDeviceId) && Boolean(connectionId);
  return {
    kind: ready ? (runtime === 'esp-p4' ? 'p4' : 'legacy') : 'waiting',
    key: JSON.stringify([ready, runtime, usb?.boardDeviceId || '', bootId, connectionId, agentId]),
  };
}
export function createVoiceRuntimePoller({target, invoke, publish, isCurrent}) {
  let cancelled = false, inFlight = false;
  const current = () => !cancelled && isCurrent();
  return {
    cancel() { cancelled = true; },
    async poll() {
      if (!current() || inFlight) return;
      if (target.kind === 'waiting') {
        publish({mode:'waiting_device',running:false,message:'正在等待设备握手确认',agentId:'',targetKey:target.key});
        return;
      }
      inFlight = true;
      try {
        const result = target.kind === 'p4'
          ? await invoke('ensure_device_voice_runtime', {input:{interactive:false}})
          : await invoke('ensure_voice_runtime');
        if (!current()) return;
        publish({mode:result?.mode || null,message:result?.message || '',running:!!result?.running,
          agentId:result?.selectedAgentId || result?.profile?.selectedAgentId || '',targetKey:target.key});
      } catch (error) {
        if (current()) publish({mode:'error',running:false,message:String(error),agentId:'',targetKey:target.key});
      } finally { inFlight = false; }
    },
  };
}
