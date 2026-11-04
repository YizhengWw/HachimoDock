import React from 'react';
import { invoke } from '@tauri-apps/api/core';

let optionalEnsureInFlight = null;
export function ensureOptionalDshDesktop() {
  if (!optionalEnsureInFlight) {
    // Replace an old pair dock with the optional status-only module. Installation
    // failure is deliberately independent of desktop input readiness.
    const promise = Promise.resolve().then(() => invoke('connect_dsh_desktop'))
      .then(() => undefined, () => undefined);
    optionalEnsureInFlight = promise;
    promise.then(() => { if (optionalEnsureInFlight === promise) optionalEnsureInFlight = null; });
  }
  return optionalEnsureInFlight;
}

// Optional informational view only. Device voice uses the shared desktop
// composer adapter; plugin connectivity is never an input prerequisite.
export default function DshConnectionPanel() {
  return <section className="voice-panel__advanced" aria-label="DeepSeek 桌面输入">
    <strong>DeepSeek 桌面输入</strong>
    <p>打开目标对话并保持当前输入框可编辑。语音先写入草稿，确认后发送。</p>
  </section>;
}
