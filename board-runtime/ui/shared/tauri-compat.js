(function bootstrapTauriCompat() {
  const BOOTSTRAP_FLAG = '__PET_CLAW_DESKTOP_API_READY__';
  const FRONTEND_READY_FLAG = '__PET_CLAW_FRONTEND_READY_SENT__';
  const MAX_BOOTSTRAP_ATTEMPTS = 120;
  let bootstrapAttempts = 0;

  function tryBootstrap() {
    if (window[BOOTSTRAP_FLAG]) {
      return;
    }

    const tauri = window.__TAURI__;
    if (!tauri || !tauri.core || typeof tauri.core.invoke !== 'function') {
      bootstrapAttempts += 1;
      if (bootstrapAttempts < MAX_BOOTSTRAP_ATTEMPTS) {
        window.setTimeout(tryBootstrap, 50);
      }
      return;
    }

    window[BOOTSTRAP_FLAG] = true;

    const invokeNative = tauri.core.invoke;
    const convertFileSrc = typeof tauri.core.convertFileSrc === 'function'
      ? tauri.core.convertFileSrc
      : null;
    const eventApi = tauri.event || null;
    const webviewApi = tauri.webviewWindow || tauri.window || null;
    const currentWindow = webviewApi?.getCurrentWebviewWindow?.() || webviewApi?.getCurrentWindow?.() || null;

    const MAIN_SEND_CHANNELS = new Set();

    const MAIN_INVOKE_CHANNELS = new Set([
      'pet-claw-send',
      'get-websocket-port',
      'get-websocket-config',
      'get-onboarding-defaults',
      'get-onboarding-state',
      'run-onboarding-action',
      'window-control'
    ]);

    const MAIN_ON_CHANNELS = new Set([
      'new-message',
      'agent-response',
      'screen-input-action'
    ]);

    let runtimePid = 0;
    let petAssetsRootPromise = null;

    function toSerializableArgs(args) {
      return Array.isArray(args) ? args : [];
    }

    async function bridgeInvoke(channel, args) {
      return invokeNative('bridge_invoke', {
        request: {
          channel,
          args: toSerializableArgs(args)
        }
      });
    }

    async function bridgeSend(channel, args) {
      return invokeNative('bridge_send', {
        request: {
          channel,
          args: toSerializableArgs(args)
        }
      });
    }

    function getPetAssetsRoot() {
      if (!petAssetsRootPromise) {
        petAssetsRootPromise = invokeNative('pet_assets_root').catch((error) => {
          petAssetsRootPromise = null;
          throw error;
        });
      }
      return petAssetsRootPromise;
    }

    function listenOnWindow(channel, callback) {
      if (currentWindow && typeof currentWindow.listen === 'function') {
        let unlisten = null;
        currentWindow.listen(channel, (event) => callback(event.payload)).then((fn) => {
          unlisten = fn;
        }).catch((error) => {
          console.error(`[tauri-compat] listen failed for ${channel}:`, error);
        });
        return () => {
          if (typeof unlisten === 'function') {
            unlisten();
          }
        };
      }

      if (eventApi && typeof eventApi.listen === 'function') {
        let unlisten = null;
        eventApi.listen(channel, (event) => callback(event.payload)).then((fn) => {
          unlisten = fn;
        }).catch((error) => {
          console.error(`[tauri-compat] global listen failed for ${channel}:`, error);
        });
        return () => {
          if (typeof unlisten === 'function') {
            unlisten();
          }
        };
      }

      return () => {};
    }

    window.desktopAPI = {
      send(channel, data) {
        if (!MAIN_SEND_CHANNELS.has(channel)) return;
        bridgeSend(channel, [data]).catch((error) => {
          console.error(`[tauri-compat] send failed for ${channel}:`, error);
        });
      },
      invoke(channel, ...args) {
        if (!MAIN_INVOKE_CHANNELS.has(channel)) {
          return Promise.reject(new Error(`Invalid channel: ${channel}`));
        }
        return bridgeInvoke(channel, args);
      },
      on(channel, callback) {
        if (!MAIN_ON_CHANNELS.has(channel)) return () => {};
        return listenOnWindow(channel, callback);
      },
      get pid() {
        return runtimePid;
      },
      get isWindows() {
        return navigator.userAgent.includes('Windows');
      },
      getPetAssetsRoot,
      toAssetUrl(filePath) {
        if (!filePath) return '';
        if (convertFileSrc) {
          return convertFileSrc(filePath, 'asset');
        }
        return filePath;
      },
      windowControl(action, label) {
        const normalizedAction = String(action || '').trim();
        const normalizedLabel = String(label || currentWindow?.label || 'main').trim() || 'main';
        const channelByAction = {
          minimize: 'window.minimize',
          toggleMaximize: 'window.toggle_maximize',
          close: 'window.close',
          show: 'window.show',
          hide: 'window.hide',
          focus: 'window.focus'
        };
        const channel = channelByAction[normalizedAction];
        if (!channel) {
          return Promise.reject(new Error(`Invalid window action: ${normalizedAction}`));
        }
        return window.desktopAPI.invoke('window-control', {
          method: channel,
          params: {
            label: normalizedLabel
          }
        });
      }
    };

    invokeNative('runtime_pid')
      .then((pid) => {
        runtimePid = Number(pid) || 0;
      })
      .catch(() => {});

    if (currentWindow && typeof currentWindow.close === 'function') {
      window.close = () => currentWindow.close();
    }

    function notifyFrontendReady() {
      if (window[FRONTEND_READY_FLAG]) {
        return;
      }
      window[FRONTEND_READY_FLAG] = true;
      invokeNative('frontend_ready').catch((error) => {
        console.error('[tauri-compat] frontend_ready failed:', error);
      });
    }

    window.dispatchEvent(new CustomEvent('desktop-api-ready'));

    if (document.readyState === 'loading') {
      window.addEventListener('DOMContentLoaded', notifyFrontendReady, { once: true });
    } else {
      notifyFrontendReady();
    }
  }

  tryBootstrap();
})();
