(function attachPetLifecycleResolver(root) {
    // idle 是最低优先级状态 — 永远是 passive 的。
    // 只有显式生命周期事件（Stop, SessionEnd 等）才能主动切换动画。
    // state-level 的 idle 帧永远不会打断更高优先级的动画。

    function createAction(abstractState, command, reason) {
        return {
            abstractState,
            command,
            reason: String(reason || abstractState || "").trim()
        };
    }

    function createWorkingAction(reason) {
        return createAction("working", { type: "startWorking" }, reason || "working");
    }

    function shouldRetainWorkingThroughIdle(options = {}) {
        const actionAbstractState = String(options.actionAbstractState || "").trim().toLowerCase();
        if (actionAbstractState !== "idle" && !actionAbstractState.startsWith("idle.")) {
            return false;
        }

        const lastWorkingAtMs = Number(options.lastWorkingAtMs);
        const nowMs = Number(options.nowMs);
        if (!Number.isFinite(lastWorkingAtMs) || lastWorkingAtMs <= 0 || !Number.isFinite(nowMs)) {
            return false;
        }

        const bufferMs = Math.max(0, Number(options.workingIdleBufferMs) || 3000);
        const elapsedMs = nowMs - lastWorkingAtMs;
        if (elapsedMs < 0 || elapsedMs > bufferMs) {
            return false;
        }

        const activeBridgeAbstractState = String(options.activeBridgeAbstractState || "").trim().toLowerCase();
        return activeBridgeAbstractState === "working" || Boolean(options.isWorkingAnimationActive);
    }

    function resolvePetLifecycleAction(payload) {
        const lifecycleEvent = String(payload?.event || "").trim();
        const normalizedState = String(payload?.state || "").trim().toLowerCase();
        const normalizedReason = String(payload?.reason || "").trim().toLowerCase();

        // 显式生命周期事件 — 最高优先级，直接映射
        const byEvent = {
            SessionStart: createAction("idle.default", {
                type: "startIdle",
                poolKey: "default",
                options: { forceReselection: true, force: true }
            }, "SessionStart"),
            UserPromptSubmit: createWorkingAction("UserPromptSubmit"),
            PreToolUse: createWorkingAction("PreToolUse"),
            AssistantMessage: createAction("done", {
                type: "startFinishThenSpeaking",
                options: {
                    videoTransition: "black-fade"
                }
            }, "AssistantMessage"),
            SubagentStart: createWorkingAction("SubagentStart"),
            SubagentStop: createWorkingAction("SubagentStop"),
            PreCompact: createWorkingAction("PreCompact"),
            WorktreeCreate: createWorkingAction("WorktreeCreate"),
            PostCompact: createAction("done", {
                type: "startFinish",
                options: {
                    videoTransition: "black-fade"
                }
            }, "PostCompact"),
            Notification: createAction("notification", { type: "startNotification" }, "Notification"),
            Elicitation: createAction("waiting_user", { type: "startWaitingUser" }, "Elicitation"),
            PermissionRequest: createAction("waiting_user", { type: "startWaitingUser" }, "PermissionRequest"),
            PostToolUseFailure: createAction("error", { type: "startError" }, "PostToolUseFailure"),
            StopFailure: createAction("error", { type: "startError" }, "StopFailure"),
            Stop: createAction("done", {
                type: "startFinish",
                options: {
                    videoTransition: "black-fade"
                }
            }, "Stop"),
            SessionEnd: createAction("idle.low_energy", {
                type: "startIdle",
                poolKey: "low_energy",
                options: { forceReselection: true, force: true }
            }, "SessionEnd")
        };

        if (lifecycleEvent === "PostToolUse") {
            return createWorkingAction("PostToolUse");
        }

        if (byEvent[lifecycleEvent]) {
            return byEvent[lifecycleEvent];
        }

        // state-level 映射 — 非 idle 状态正常映射
        if (normalizedState === "thinking") {
            return createWorkingAction("thinking");
        }

        if (normalizedState === "tool_running") {
            return createWorkingAction("tool_running");
        }

        if (normalizedState === "notification") {
            return createAction("notification", { type: "startNotification" }, "notification");
        }

        if (normalizedState === "speaking") {
            return createAction("speaking", { type: "startSpeaking" }, "speaking");
        }

        if (normalizedState === "done") {
            return createAction("done", {
                type: "startFinish",
                options: {
                    videoTransition: "black-fade"
                }
            }, "done");
        }

        if (normalizedState === "error") {
            return createAction("error", { type: "startError" }, "error");
        }

        if (normalizedState === "waiting_user") {
            return createAction("waiting_user", { type: "startWaitingUser" }, "waiting_user");
        }

        // idle / active / 未知 — 全部走 passive idle
        // idle 是兜底状态，永远不主动打断任何动画
        if (normalizedState === "idle" || normalizedState === "active" || normalizedReason.startsWith("active.")) {
            return createAction("idle.default", {
                type: "startIdle",
                poolKey: "default",
                passive: true,
                options: {
                    forceReselection: false,
                    force: false
                }
            }, "idle.passive");
        }

        return null;
    }

    root.resolvePetLifecycleAction = resolvePetLifecycleAction;
    root.shouldRetainWorkingThroughIdle = shouldRetainWorkingThroughIdle;

    if (typeof module !== "undefined" && module.exports) {
        module.exports = {
            resolvePetLifecycleAction,
            shouldRetainWorkingThroughIdle
        };
    }
})(typeof window !== "undefined" ? window : globalThis);
