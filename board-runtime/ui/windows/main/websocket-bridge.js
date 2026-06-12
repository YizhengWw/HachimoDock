(function attachPetClawWebSocketBridge(root) {
    const DEFAULT_WEBSOCKET_HOST = "127.0.0.1";
    const DEFAULT_WEBSOCKET_PORT = 0;
    const DEFAULT_RETRY_DELAY = 5000;

    function parsePositivePort(value) {
        const parsed = Number.parseInt(value, 10);
        return Number.isInteger(parsed) && parsed > 0 && parsed <= 65535 ? parsed : 0;
    }

    function createPetClawWebSocketBridge(options = {}) {
        const logger = options.logger || console;
        const retryDelay = Number.isFinite(options.retryDelay) ? options.retryDelay : DEFAULT_RETRY_DELAY;
        let ws = null;
        let websocketConfig = null;

        function getDesktopApi() {
            if (typeof options.desktopAPI === "function") {
                return options.desktopAPI() || null;
            }
            return options.desktopAPI || root.desktopAPI || null;
        }

        async function getWebSocketConfig() {
            if (websocketConfig) {
                return websocketConfig;
            }

            const desktopAPI = getDesktopApi();
            if (!desktopAPI || typeof desktopAPI.invoke !== "function") {
                return null;
            }

            try {
                const config = await desktopAPI.invoke("get-websocket-config");
                websocketConfig = {
                    host: config?.host || DEFAULT_WEBSOCKET_HOST,
                    port: parsePositivePort(config?.port) || parsePositivePort(DEFAULT_WEBSOCKET_PORT)
                };

                if (!websocketConfig.host || !websocketConfig.port) {
                    websocketConfig = null;
                }

                return websocketConfig;
            } catch (error) {
                logger.warn("获取 WebSocket 配置失败，稍后重试", error);
                websocketConfig = null;
                return null;
            }
        }

        async function connect() {
            const config = await getWebSocketConfig();
            const host = config?.host || DEFAULT_WEBSOCKET_HOST;
            const port = parsePositivePort(config?.port);
            const endpoint = `ws://${host}:${port}`;

            if (!host || !port) {
                logger.warn("WebSocket 配置尚未可用，等待后续重试");
                ws = null;
                root.setTimeout(connect, retryDelay);
                return;
            }

            ws = new root.WebSocket(endpoint);

            ws.onopen = () => {
                logger.log(`WebSocket 已连接: ${endpoint}`);
            };

            ws.onmessage = (event) => {
                let data = null;
                try {
                    data = JSON.parse(event.data);
                } catch (error) {
                    logger.error("WebSocket 消息解析失败:", error);
                    return;
                }

                if (data && typeof data === "object") {
                    if (data.type === "bridge_state") {
                        const now = Date.now();
                        const wsSentAt = Number(data._wsSentAt);
                        const bridgeReceivedAt = Number(data._bridgeReceivedAt);
                        const payloadTsMs = Number(data.payload?.tsMs);
                        const wsToRendererMs = Number.isFinite(wsSentAt) ? Math.max(0, now - wsSentAt) : undefined;
                        const bridgeToWsMs = Number.isFinite(bridgeReceivedAt) && Number.isFinite(wsSentAt)
                            ? Math.max(0, wsSentAt - bridgeReceivedAt)
                            : undefined;
                        const sourceToFrontendMs = Number.isFinite(payloadTsMs) ? Math.max(0, now - payloadTsMs) : undefined;
                        logger.info("[ws-bridge] 收到 bridge_state", {
                            seq: data._wsSeq || 0,
                            replayed: Boolean(data.replayed),
                            topic: data.topic || "",
                            source: data.payload?.source || "",
                            state: data.payload?.state || "",
                            rawState: data.payload?.rawState || "",
                            event: data.payload?.event || "",
                            reason: data.payload?.reason || "",
                            sessionId: data.payload?.sessionId || "",
                            bridgeReceivedAt: Number.isFinite(bridgeReceivedAt) ? bridgeReceivedAt : null,
                            bridgePayloadTsMs: Number.isFinite(payloadTsMs) ? payloadTsMs : null,
                            sourceToBridgeMs: Number.isFinite(data._bridgeAgeMs) ? data._bridgeAgeMs : undefined,
                            bridgeToWsMs: Number.isFinite(bridgeToWsMs) ? bridgeToWsMs : undefined,
                            wsToRendererMs,
                            sourceToRendererMs: sourceToFrontendMs
                        });
                    } else if (data.type === "bridge_snapshot") {
                        logger.info("[ws-bridge] 收到 bridge_snapshot", {
                            seq: data._wsSeq || 0,
                            replayed: Boolean(data.replayed),
                            activeTopic: data.activeFrame?.topic || "",
                            activeSource: data.activeFrame?.payload?.source || "",
                            activeState: data.activeFrame?.payload?.state || "",
                            sourceCount: Array.isArray(data.sourceFrames) ? data.sourceFrames.length : 0
                        });
                    } else if (data.type === "screen_input_action") {
                        logger.info("[ws-bridge] 收到 screen_input_action", {
                            seq: data._wsSeq || 0,
                            actionId: data.actionId || "",
                            topic: data.topic || "",
                            boardDeviceId: data.payload?.boardDeviceId || data.action?.boardDeviceId || "",
                            actionType: data.payload?.type || data.action?.type || ""
                        });
                    } else {
                        logger.info("[ws-bridge] 收到消息", {
                            type: data.type || "unknown",
                            seq: data._wsSeq || 0
                        });
                    }
                }

                if (typeof options.onMessage === "function") {
                    options.onMessage(data);
                }
            };

            ws.onerror = (error) => {
                logger.error("WebSocket 错误:", error);
            };

            ws.onclose = () => {
                root.setTimeout(connect, retryDelay);
            };
        }

        return {
            connect,
            getSocket() {
                return ws;
            },
            getWebSocketConfig,
            resetConfigCache() {
                websocketConfig = null;
            }
        };
    }

    root.createPetClawWebSocketBridge = createPetClawWebSocketBridge;
})(typeof window !== "undefined" ? window : globalThis);
