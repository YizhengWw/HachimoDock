(() => {
  const DEFAULT_WS_PORT = 80;
  const RUNTIME_CONFIG_TTL_MS = 1500;

  function parsePort(value, fallback = DEFAULT_WS_PORT) {
    const parsed = Number.parseInt(value, 10);
    return Number.isInteger(parsed) && parsed > 0 && parsed <= 65535 ? parsed : fallback;
  }

  async function readRuntimeConfig() {
    try {
      const response = await fetch('/board-runtime-config.json', { cache: 'no-store' });
      if (!response.ok) return {};
      return await response.json();
    } catch {
      return {};
    }
  }

  let runtimeConfigCache = null;
  let runtimeConfigFetchedAt = 0;

  async function getRuntimeConfig(forceRefresh = false) {
    const now = Date.now();
    if (!forceRefresh && runtimeConfigCache && (now - runtimeConfigFetchedAt) < RUNTIME_CONFIG_TTL_MS) {
      return runtimeConfigCache;
    }
    runtimeConfigCache = await readRuntimeConfig();
    runtimeConfigFetchedAt = now;
    return runtimeConfigCache;
  }

  function buildOnboardingState(config) {
    const pairingState = String(config?.pairingState || "").trim();
    const pairingMode = String(config?.pairingMode || "").trim();
    const boardDeviceId = String(config?.boardDeviceId || "").trim();
    const apIp = String(config?.apIp || "192.168.44.1").trim();
    const hint = String(config?.pairingHint || "").trim();
    const ready = pairingState === "sta_ready";

    if (ready) {
      return {
        ready: true,
        runtimeReady: true,
        mqttReady: true,
        localAgentReady: false,
        remoteAgentReady: true,
        pairing: {
          state: "paired",
          stateLabel: "已连接",
          boardLabel: boardDeviceId || "Board Runtime",
          detail: "设备已处于 Station 模式并可接收状态流。"
        },
        checks: [
          { ok: true, label: "设备网络", detail: "已完成配网并进入 STA 模式" },
          { ok: true, label: "运行时服务", detail: "HTTP/WebSocket 服务已就绪" }
        ],
        recommendedCommands: []
      };
    }

    if (pairingMode === "ap") {
      return {
        ready: false,
        runtimeReady: true,
        mqttReady: false,
        localAgentReady: false,
        remoteAgentReady: true,
        pairing: {
          state: "ap_fallback",
          stateLabel: "热点模式",
          boardLabel: boardDeviceId || "Board Runtime",
          detail: hint || `请连接设备热点后访问 ${apIp} 继续配置。`
        },
        checks: [
          { ok: true, label: "设备待配网", detail: "已开启 AP 热点回退模式" },
          { ok: false, label: "电脑端发现", detail: "请在 Pet Manager 中连接设备热点并下发配置" }
        ],
        recommendedCommands: [`连接热点并访问 ${apIp}`]
      };
    }

    return {
      ready: false,
      runtimeReady: true,
      mqttReady: false,
      localAgentReady: false,
      remoteAgentReady: true,
      pairing: {
        state: pairingState || "waiting_config",
        stateLabel: "等待配对",
        boardLabel: boardDeviceId || "Board Runtime",
        detail: hint || "请在电脑端打开 Pet Manager 扫描设备。"
      },
      checks: [
        { ok: true, label: "设备待配网", detail: "设备已进入局域网发现阶段" },
        { ok: false, label: "发现状态", detail: "等待 Pet Manager 扫描并配置" }
      ],
      recommendedCommands: ["打开 Pet Manager 并开始设备发现"]
    };
  }

  async function postInputAction(action) {
    const response = await fetch('/input/action', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json'
      },
      body: JSON.stringify(action || {})
    });
    let payload = null;
    try {
      payload = await response.json();
    } catch {
      payload = { ok: response.ok };
    }
    if (!response.ok) {
      return {
        ok: false,
        status: response.status,
        ...(payload || {})
      };
    }
    return payload || { ok: true };
  }

  window.desktopAPI = {
    send() {},
    async invoke(channel, ...args) {
      if (channel === 'get-websocket-config') {
        const config = await getRuntimeConfig();
        return {
          host: config.websocketHost || window.location.hostname || '127.0.0.1',
          port: parsePort(config.websocketPort || window.location.port)
        };
      }
      if (channel === 'get-onboarding-defaults') {
        const config = await getRuntimeConfig();
        const pairingMode = String(config?.pairingMode || "").trim();
        const title = pairingMode === "ap" ? "设备已进入热点模式" : "正在等待配对中";
        const description = pairingMode === "ap"
          ? `请连接设备热点并访问 ${String(config?.apIp || "192.168.44.1")}`
          : "桌宠当前还没有完成首次配对，请启动 Pet Manager 继续配置。";
        return {
          runtimeReady: true,
          mqttReady: pairingMode === "sta",
          title: 'Board runtime',
          recommendedCommand: "打开 Pet Manager 并完成设备发现",
          productName: "Board Runtime",
          petManagerName: "Pet Manager",
          readyMessage: title,
          description
        };
      }
      if (channel === 'get-onboarding-state') {
        const config = await getRuntimeConfig(true);
        return buildOnboardingState(config);
      }
      if (channel === 'run-onboarding-action') {
        return { ok: false, message: 'Board runtime does not launch Pet Manager.' };
      }
      if (channel === 'pet-screen-input-action') {
        return postInputAction(args[0] || {});
      }
      if (channel === 'pet-claw-send') {
        return { ok: false, error: 'Board runtime does not run local OpenClaw chat; touch actions are forwarded separately.' };
      }
      if (channel === 'window-control') {
        return false;
      }
      return null;
    },
    on() {
      return () => {};
    },
    get pid() {
      return 0;
    },
    get isWindows() {
      return false;
    },
    get isBoardRuntime() {
      return true;
    },
    publishInputAction(action) {
      return postInputAction(action);
    },
    async getPetAssetsRoot() {
      return '';
    },
    toAssetUrl(filePath) {
      return filePath || '';
    },
    windowControl() {
      return Promise.resolve(false);
    }
  };

  window.dispatchEvent(new CustomEvent('desktop-api-ready'));
})();
