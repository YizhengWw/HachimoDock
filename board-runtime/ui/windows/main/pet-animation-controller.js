(function attachPetAnimationController(root) {
    function normalizeText(value, fallback = "") {
        const text = value == null ? "" : String(value);
        return text.trim() || fallback;
    }

    function createPetAnimationController(options = {}) {
        const helpers = options.helpers || {};
        const onPlay = typeof options.onPlay === "function" ? options.onPlay : () => {};
        const onReplay = typeof options.onReplay === "function" ? options.onReplay : () => {};
        const onStateChange = typeof options.onStateChange === "function" ? options.onStateChange : () => {};
        const random = typeof options.random === "function" ? options.random : Math.random;
        const defaultIdleState = normalizeText(options.defaultIdleState, "idle-enter");
        const initialState = options.initialState || {};
        const genericWorkingFamilies = ["thinking", "typing", "browsing"];
        const state = {
            currentState: normalizeText(initialState.currentState, defaultIdleState),
            targetState: normalizeText(initialState.targetState, defaultIdleState),
            hasToolActivity: Boolean(initialState.hasToolActivity),
            activeIdleFamily: normalizeText(initialState.activeIdleFamily),
            activeIdlePoolKey: normalizeText(initialState.activeIdlePoolKey, "default"),
            idleLoopPlaysRemaining: Number.isFinite(initialState.idleLoopPlaysRemaining)
                ? Number(initialState.idleLoopPlaysRemaining)
                : 0,
            queuedStateAfterIdle: normalizeText(initialState.queuedStateAfterIdle),
            queuedIdleFamilyAfterExit: normalizeText(initialState.queuedIdleFamilyAfterExit),
            activeWorkingFamily: normalizeText(initialState.activeWorkingFamily),
            queuedWorkingFamily: normalizeText(initialState.queuedWorkingFamily),
            activeWorkingTransitionFromFamily: normalizeText(initialState.activeWorkingTransitionFromFamily),
            activeWorkingTransitionToFamily: normalizeText(initialState.activeWorkingTransitionToFamily),
            workingLoopRepeats: Number.isFinite(initialState.workingLoopRepeats)
                ? Number(initialState.workingLoopRepeats)
                : 0,
            queuedStateAfterWorking: normalizeText(initialState.queuedStateAfterWorking),
            queuedStateAfterSpeaking: normalizeText(initialState.queuedStateAfterSpeaking),
            speakingLoopRepeats: Number.isFinite(initialState.speakingLoopRepeats)
                ? Number(initialState.speakingLoopRepeats)
                : 0,
            activeBridgeAbstractState: normalizeText(initialState.activeBridgeAbstractState, "idle.default"),
            pendingDoneAfterWorking: Boolean(initialState.pendingDoneAfterWorking),
            pendingDoneTransition: normalizeText(initialState.pendingDoneTransition)
        };

        function getState() {
            return {
                ...state
            };
        }

        function emitStateChange(reason = "") {
            onStateChange(getState(), reason);
        }

        function logAnimationTrace(event, details = {}) {
            if (typeof helpers.logAnimationTrace === "function") {
                helpers.logAnimationTrace(event, details);
            }
        }

        function logBridgeTrace(event, details = {}) {
            if (typeof helpers.logBridgeTrace === "function") {
                helpers.logBridgeTrace(event, details);
            }
        }

        function resolvePlannedVideoSource(nextState) {
            if (typeof helpers.resolvePlannedVideoSource !== "function") {
                return "";
            }
            return helpers.resolvePlannedVideoSource(nextState);
        }

        function getCurrentAgentIdleState() {
            if (typeof helpers.getCurrentAgentIdleState !== "function") {
                return defaultIdleState;
            }
            return normalizeText(helpers.getCurrentAgentIdleState(), defaultIdleState);
        }

        function getCurrentAgentIdleLoopStates() {
            if (typeof helpers.getCurrentAgentIdleLoopStates !== "function") {
                return null;
            }
            const loopStates = helpers.getCurrentAgentIdleLoopStates();
            return Array.isArray(loopStates)
                ? loopStates.filter((value) => normalizeText(value))
                : null;
        }

        function getStateFlow() {
            if (typeof helpers.getStateFlow !== "function") {
                return {};
            }
            return helpers.getStateFlow() || {};
        }

        function normalizePlaybackTransition(commandOptions = {}) {
            const normalizedTransition = normalizeText(commandOptions.videoTransition);
            return normalizedTransition === "fade" || normalizedTransition === "black-fade"
                ? normalizedTransition
                : "";
        }

        function buildPlaybackOptions(commandOptions = {}) {
            const videoTransition = normalizePlaybackTransition(commandOptions);
            return videoTransition ? { videoTransition } : {};
        }

        function isIdleFamilyPhaseState(nextState) {
            return typeof helpers.isIdleFamilyPhaseState === "function"
                ? helpers.isIdleFamilyPhaseState(nextState)
                : false;
        }

        function isSpeakingPhaseState(nextState) {
            return typeof helpers.isSpeakingPhaseState === "function"
                ? helpers.isSpeakingPhaseState(nextState)
                : false;
        }

        function isWorkingFamilyPhaseState(nextState) {
            return typeof helpers.isWorkingFamilyPhaseState === "function"
                ? helpers.isWorkingFamilyPhaseState(nextState)
                : false;
        }

        function isIdleLikeState(nextState) {
            return typeof helpers.isIdleLikeState === "function"
                ? helpers.isIdleLikeState(nextState)
                : false;
        }

        function pickIdleFamily(poolKey = "default", fallbackFamily = state.activeIdleFamily) {
            if (typeof helpers.pickIdleFamily !== "function") {
                return "";
            }
            return normalizeText(helpers.pickIdleFamily(poolKey, fallbackFamily));
        }

        function pickWorkingFamily(preferredNames = null, fallbackFamily = state.activeWorkingFamily) {
            if (typeof helpers.pickWorkingFamily !== "function") {
                return "";
            }
            return normalizeText(helpers.pickWorkingFamily(preferredNames, fallbackFamily));
        }

        function resolveWorkingTransitionSource(fromFamily, toFamily) {
            if (typeof helpers.resolveWorkingTransitionSource !== "function") {
                return "";
            }
            return normalizeText(helpers.resolveWorkingTransitionSource(fromFamily, toFamily));
        }

        function pickIdleLoopRepeats() {
            if (typeof helpers.pickIdleLoopRepeats !== "function") {
                return 0;
            }
            return Number(helpers.pickIdleLoopRepeats()) || 0;
        }

        function hasVideoPlaybackCompleted() {
            if (typeof helpers.hasVideoPlaybackCompleted !== "function") {
                return false;
            }
            return Boolean(helpers.hasVideoPlaybackCompleted());
        }

        function clearWorkingSelection() {
            state.activeWorkingFamily = "";
            state.queuedWorkingFamily = "";
            state.activeWorkingTransitionFromFamily = "";
            state.activeWorkingTransitionToFamily = "";
            state.workingLoopRepeats = 0;
            state.pendingDoneAfterWorking = false;
            state.pendingDoneTransition = "";
        }

        function clearIdleSelection() {
            state.activeIdleFamily = "";
            state.activeIdlePoolKey = "default";
            state.idleLoopPlaysRemaining = 0;
            state.queuedStateAfterIdle = "";
            state.queuedIdleFamilyAfterExit = "";
        }

        function shouldDeferStateSwitch(nextState) {
            const normalizedCurrentState = normalizeText(state.currentState);
            const normalizedNextState = normalizeText(nextState);
            if (!normalizedCurrentState || !normalizedNextState || normalizedCurrentState === normalizedNextState) {
                return false;
            }

            return isIdleFamilyPhaseState(normalizedCurrentState)
                || isSpeakingPhaseState(normalizedCurrentState)
                || isWorkingFamilyPhaseState(normalizedCurrentState);
        }

        function playStateNow(nextState, playbackMeta = {}) {
            const normalizedState = normalizeText(nextState);
            if (!normalizedState) {
                return false;
            }

            const previousTargetState = state.targetState;
            state.currentState = normalizedState;
            state.targetState = normalizedState;
            logAnimationTrace("playStateNow", {
                previousTargetState,
                nextState: normalizedState,
                source: resolvePlannedVideoSource(normalizedState)
            });
            emitStateChange("playStateNow");
            onPlay(normalizedState, getState(), buildPlaybackOptions(playbackMeta));
            return true;
        }

        function isWorkingFlowActive() {
            return isWorkingFamilyPhaseState(state.currentState)
                || isWorkingFamilyPhaseState(state.targetState)
                || String(state.currentState || "").includes("working")
                || String(state.targetState || "").includes("working");
        }

        function setPendingDoneAfterWorking(commandOptions = {}) {
            state.pendingDoneAfterWorking = true;
            state.pendingDoneTransition = normalizePlaybackTransition(commandOptions);
            emitStateChange("setPendingDoneAfterWorking");
        }

        function clearPendingDoneAfterWorking() {
            state.pendingDoneAfterWorking = false;
            state.pendingDoneTransition = "";
            emitStateChange("clearPendingDoneAfterWorking");
        }

        function startDeferredFinishAfterWorking() {
            const transition = normalizePlaybackTransition({
                videoTransition: state.pendingDoneTransition || "black-fade"
            });
            clearPendingDoneAfterWorking();
            state.hasToolActivity = false;
            state.queuedStateAfterWorking = "";
            state.queuedWorkingFamily = "";
            state.workingLoopRepeats = 0;
            state.activeWorkingFamily = "finish";
            emitStateChange("startDeferredFinishAfterWorking");
            return playStateNow("working-enter", { videoTransition: transition || "black-fade" });
        }

        function setTargetState(nextState, commandOptions = {}) {
            const normalizedState = normalizeText(nextState);
            const force = Boolean(commandOptions.force);
            if (!normalizedState) {
                logBridgeTrace("setTargetState:skip-empty", {
                    requestedState: nextState,
                    force
                });
                return false;
            }

            if (!force && state.targetState === normalizedState) {
                logBridgeTrace("setTargetState:skip-same-target", {
                    requestedState: normalizedState,
                    force
                });
                return false;
            }

            const previousTargetState = state.targetState;
            state.targetState = normalizedState;
            const shouldPlay = force
                || (!shouldDeferStateSwitch(normalizedState) && !String(state.currentState || "").startsWith(normalizedState));
            if (shouldPlay) {
                state.currentState = normalizedState;
            }
            logAnimationTrace("setTargetState", {
                previousTargetState,
                nextTarget: normalizedState,
                force,
                defer: shouldDeferStateSwitch(normalizedState),
                currentStatePrefixMatch: String(state.currentState || "").startsWith(normalizedState)
            });
            emitStateChange("setTargetState");
            if (shouldPlay) {
                onPlay(normalizedState, getState(), buildPlaybackOptions(commandOptions));
            }
            return true;
        }

        function syncCurrentState(nextState) {
            const normalizedState = normalizeText(nextState);
            if (!normalizedState) {
                return false;
            }
            state.currentState = normalizedState;
            if (!state.targetState) {
                state.targetState = normalizedState;
            }
            emitStateChange("syncCurrentState");
            return true;
        }

        function setActiveBridgeAbstractState(nextState) {
            state.activeBridgeAbstractState = normalizeText(nextState, state.activeBridgeAbstractState);
            emitStateChange("setActiveBridgeAbstractState");
        }

        function resetWorkingFamilySelection() {
            clearWorkingSelection();
            emitStateChange("resetWorkingFamilySelection");
        }

        function resetIdleFamilySelection() {
            clearIdleSelection();
            emitStateChange("resetIdleFamilySelection");
        }

        function resetRunState() {
            state.hasToolActivity = false;
            state.queuedStateAfterWorking = "";
            state.queuedStateAfterSpeaking = "";
            state.queuedStateAfterIdle = "";
            state.speakingLoopRepeats = 0;
            clearWorkingSelection();
            clearPendingDoneAfterWorking();
            emitStateChange("resetRunState");
        }

        function exitIdleAnimation(nextState = "idle-enter") {
            const fallbackState = normalizeText(nextState, "idle-enter");
            state.queuedStateAfterIdle = fallbackState;

            if (!isIdleFamilyPhaseState(state.currentState) && !isIdleFamilyPhaseState(state.targetState)) {
                logBridgeTrace("exitIdleAnimation:play-immediately", {
                    fallbackState
                });
                return playStateNow(fallbackState);
            }

            if (state.currentState === "idle-exit" || state.targetState === "idle-exit") {
                logBridgeTrace("exitIdleAnimation:awaiting-exit", {
                    fallbackState
                });
                emitStateChange("exitIdleAnimation:awaiting-exit");
                return true;
            }

            return setTargetState("idle-exit");
        }

        function startIdleAnimation(poolKey = "default", commandOptions = {}) {
            const desiredFamily = pickIdleFamily(poolKey, commandOptions.forceReselection ? "" : state.activeIdleFamily);
            const normalizedPoolKey = normalizeText(poolKey, "default");
            const skipWorkingRedirect = Boolean(commandOptions.skipWorkingRedirect);
            logBridgeTrace("startIdleAnimation", {
                poolKey,
                desiredFamily,
                forceReselection: Boolean(commandOptions.forceReselection),
                force: Boolean(commandOptions.force),
                skipWorkingRedirect
            });

            if (!desiredFamily) {
                logBridgeTrace("startIdleAnimation:missing-family", {
                    poolKey
                });
                const preferredIdleState = getCurrentAgentIdleState();
                if (preferredIdleState) {
                    return setTargetState(preferredIdleState, { force: Boolean(commandOptions.force) });
                }
                emitStateChange("startIdleAnimation:missing-family");
                onReplay();
                return true;
            }

            state.activeIdlePoolKey = normalizedPoolKey;

            if (isSpeakingPhaseState(state.currentState) || isSpeakingPhaseState(state.targetState)) {
                logBridgeTrace("startIdleAnimation:redirect-speaking", {
                    desiredFamily
                });
                return exitSpeakingAnimation("idle-enter");
            }

            if (!skipWorkingRedirect && (isWorkingFamilyPhaseState(state.currentState) || isWorkingFamilyPhaseState(state.targetState) || String(state.currentState || "").includes("working"))) {
                logBridgeTrace("startIdleAnimation:redirect-working", {
                    desiredFamily
                });
                return exitWorkingAnimation("idle-enter");
            }

            if (
                desiredFamily
                && !state.activeIdleFamily
                && (isIdleFamilyPhaseState(state.currentState) || isIdleFamilyPhaseState(state.targetState))
            ) {
                state.activeIdleFamily = desiredFamily;
                state.queuedIdleFamilyAfterExit = "";
                state.idleLoopPlaysRemaining = pickIdleLoopRepeats();
                state.queuedStateAfterIdle = "";
                logBridgeTrace("startIdleAnimation:seed-missing-family", {
                    desiredFamily,
                    currentState: state.currentState,
                    targetState: state.targetState,
                    force: Boolean(commandOptions.force)
                });
                emitStateChange("startIdleAnimation:seed-missing-family");
                return setTargetState("idle-enter", { force: true });
            }

            if (isIdleFamilyPhaseState(state.currentState) || isIdleFamilyPhaseState(state.targetState)) {
                if (state.currentState === "idle-exit" || state.targetState === "idle-exit" || commandOptions.forceReselection || state.activeIdleFamily !== desiredFamily) {
                    state.queuedIdleFamilyAfterExit = desiredFamily;
                    state.queuedStateAfterIdle = "";
                    logBridgeTrace("startIdleAnimation:queue-after-exit", {
                        desiredFamily,
                        currentState: state.currentState,
                        targetState: state.targetState
                    });
                    emitStateChange("startIdleAnimation:queue-after-exit");
                    if (state.currentState !== "idle-exit" && state.targetState !== "idle-exit") {
                        return setTargetState("idle-exit");
                    }
                    return true;
                }
            }

            if (commandOptions.forceReselection && state.currentState === "idle-loop" && state.targetState === "idle-loop") {
                logBridgeTrace("startIdleAnimation:force-exit-current-loop", {
                    desiredFamily
                });
                return setTargetState("idle-exit");
            }

            if (!commandOptions.force && state.currentState === "idle-loop" && state.targetState === "idle-loop" && state.activeIdleFamily === desiredFamily) {
                logBridgeTrace("startIdleAnimation:skip-already-looping", {
                    desiredFamily
                });
                emitStateChange("startIdleAnimation:skip-already-looping");
                return true;
            }

            state.activeIdleFamily = desiredFamily;
            state.queuedIdleFamilyAfterExit = "";
            state.idleLoopPlaysRemaining = pickIdleLoopRepeats();
            state.queuedStateAfterIdle = "";
            logBridgeTrace("startIdleAnimation:enter", {
                desiredFamily,
                idleLoopPlaysRemaining: state.idleLoopPlaysRemaining,
                force: Boolean(commandOptions.force)
            });
            emitStateChange("startIdleAnimation:enter");
            return setTargetState("idle-enter", { force: Boolean(commandOptions.force) });
        }

        function exitSpeakingAnimation(nextState = "idle-enter") {
            const fallbackState = normalizeText(nextState, "idle-enter");
            state.queuedStateAfterSpeaking = fallbackState;

            if (!isSpeakingPhaseState(state.currentState) && !isSpeakingPhaseState(state.targetState)) {
                if (fallbackState === "idle-enter") {
                    return startIdleAnimation(state.activeIdlePoolKey || "default", { forceReselection: true });
                }
                return setTargetState(fallbackState);
            }

            if (state.currentState === "speaking-exit" || state.targetState === "speaking-exit") {
                emitStateChange("exitSpeakingAnimation:awaiting-exit");
                return true;
            }

            return setTargetState("speaking-exit");
        }

        function startSpeakingAnimation(runtime = {}) {
            const activeToolCallCount = Number(runtime.activeToolCallCount) || 0;

            if (
                state.hasToolActivity
                && (
                    activeToolCallCount > 0
                    || String(state.currentState || "").includes("working")
                    || String(state.targetState || "").includes("working")
                )
            ) {
                state.queuedStateAfterWorking = "speaking-enter";
                emitStateChange("startSpeakingAnimation:queue-after-working");
                return true;
            }

            if (isIdleFamilyPhaseState(state.currentState) || isIdleFamilyPhaseState(state.targetState)) {
                state.speakingLoopRepeats = 0;
                emitStateChange("startSpeakingAnimation:redirect-idle");
                return exitIdleAnimation("speaking-enter");
            }

            if (isSpeakingPhaseState(state.currentState) || isSpeakingPhaseState(state.targetState)) {
                emitStateChange("startSpeakingAnimation:skip-already-speaking");
                return true;
            }

            state.queuedStateAfterWorking = "";
            state.queuedStateAfterSpeaking = "idle-enter";
            state.speakingLoopRepeats = 0;
            emitStateChange("startSpeakingAnimation:enter");
            return setTargetState("speaking-enter", runtime);
        }

        function startThinkingAnimation(commandOptions = {}) {
            const shouldRestart = state.activeBridgeAbstractState !== "working"
                || !genericWorkingFamilies.includes(state.activeWorkingFamily);
            return startWorkingAnimation(null, {
                ...commandOptions,
                forceReselection: shouldRestart
            });
        }

        function startWorkingFamilyTransition(nextFamily, transitionOptions = {}) {
            const normalizedNextFamily = normalizeText(nextFamily);
            const normalizedFromFamily = normalizeText(state.activeWorkingFamily);
            if (!normalizedNextFamily || normalizedNextFamily === normalizedFromFamily) {
                return false;
            }
            const transitionSource = resolveWorkingTransitionSource(normalizedFromFamily, normalizedNextFamily);
            if (transitionSource) {
                state.activeWorkingTransitionFromFamily = normalizedFromFamily;
                state.activeWorkingTransitionToFamily = normalizedNextFamily;
                state.queuedWorkingFamily = "";
                state.workingLoopRepeats = 0;
                emitStateChange("startWorkingFamilyTransition:transition");
                if (transitionOptions.immediate) {
                    return playStateNow("working-transition");
                }
                return setTargetState("working-transition");
            }
            state.activeWorkingFamily = normalizedNextFamily;
            state.queuedWorkingFamily = "";
            state.activeWorkingTransitionFromFamily = "";
            state.activeWorkingTransitionToFamily = "";
            state.workingLoopRepeats = 0;
            emitStateChange("startWorkingFamilyTransition");
            if (transitionOptions.immediate) {
                return playStateNow("working-loop");
            }
            return setTargetState("working-loop");
        }

        function startWorkingAnimation(preferredFamilies = null, commandOptions = {}) {
            clearPendingDoneAfterWorking();
            state.hasToolActivity = true;
            state.queuedStateAfterWorking = "";
            const familyPool = Array.isArray(preferredFamilies) && preferredFamilies.length
                ? preferredFamilies
                : genericWorkingFamilies;
            const desiredFamily = pickWorkingFamily(familyPool, commandOptions.forceReselection ? "" : state.activeWorkingFamily);
            const isWorkingFlowActive = isWorkingFamilyPhaseState(state.currentState) || isWorkingFamilyPhaseState(state.targetState);
            const isWorkingActive = isWorkingFlowActive
                || String(state.currentState || "").includes("working")
                || String(state.targetState || "").includes("working");

            if (!desiredFamily) {
                if (!isWorkingActive) {
                    return setTargetState("working-enter", commandOptions);
                }
                emitStateChange("startWorkingAnimation:missing-family");
                onReplay();
                return true;
            }

            if (isIdleFamilyPhaseState(state.currentState) || isIdleFamilyPhaseState(state.targetState)) {
                state.activeWorkingFamily = desiredFamily;
                state.queuedWorkingFamily = "";
                state.workingLoopRepeats = 0;
                state.queuedStateAfterIdle = "";
                state.queuedIdleFamilyAfterExit = "";
                emitStateChange("startWorkingAnimation:interrupt-idle");
                return setTargetState("working-enter", {
                    ...commandOptions,
                    force: true
                });
            }

            if (!isWorkingActive || !isWorkingFlowActive) {
                state.activeWorkingFamily = desiredFamily;
                state.queuedWorkingFamily = "";
                state.workingLoopRepeats = 0;
                emitStateChange("startWorkingAnimation:enter");
                return setTargetState("working-enter", commandOptions);
            }

            if (state.currentState === "working-transition") {
                state.queuedWorkingFamily = desiredFamily;
                emitStateChange("startWorkingAnimation:awaiting-transition");
                return true;
            }

            if (state.currentState === "working-exit") {
                state.queuedWorkingFamily = desiredFamily;
                emitStateChange("startWorkingAnimation:awaiting-exit");
                return true;
            }

            if (!state.activeWorkingFamily) {
                state.activeWorkingFamily = desiredFamily;
                state.workingLoopRepeats = 0;
            }

            if (desiredFamily !== state.activeWorkingFamily) {
                state.queuedWorkingFamily = desiredFamily;
                logBridgeTrace("startWorkingAnimation:queue-family-switch", {
                    fromFamily: state.activeWorkingFamily,
                    toFamily: desiredFamily,
                    currentState: state.currentState,
                    targetState: state.targetState
                });
                emitStateChange("startWorkingAnimation:queue-family-switch");
                if (state.targetState === "working-exit") {
                    return setTargetState("working-loop", commandOptions);
                }
                return true;
            }

            state.queuedWorkingFamily = "";
            if (state.targetState !== "working-enter" && state.targetState !== "working-loop") {
                return setTargetState("working-loop", commandOptions);
            }

            emitStateChange("startWorkingAnimation:steady");
            return true;
        }

        function startToolRunningAnimation(commandOptions = {}) {
            const enteringToolSegment = Boolean(commandOptions.forceReselection)
                || state.activeBridgeAbstractState !== "working"
                || !genericWorkingFamilies.includes(state.activeWorkingFamily);
            return startWorkingAnimation(null, {
                ...commandOptions,
                forceReselection: enteringToolSegment
            });
        }

        function startErrorAnimation(commandOptions = {}) {
            const shouldRestart = state.activeBridgeAbstractState !== "error";
            return startWorkingAnimation(["error"], {
                ...commandOptions,
                forceReselection: shouldRestart
            });
        }

        function startWaitingUserAnimation(commandOptions = {}) {
            const shouldRestart = state.activeBridgeAbstractState !== "waiting_user";
            return startWorkingAnimation(["decide"], {
                ...commandOptions,
                forceReselection: shouldRestart
            });
        }

        function startNotificationAnimation(commandOptions = {}) {
            const shouldRestart = state.activeBridgeAbstractState !== "notification";
            return startWorkingAnimation(["notification", "decide"], {
                ...commandOptions,
                forceReselection: shouldRestart
            });
        }

        function startFinishAnimation(commandOptions = {}) {
            const transition = normalizePlaybackTransition(commandOptions) || "black-fade";
            const isWorkingState = isWorkingFlowActive();

            if (isWorkingState && state.activeWorkingFamily !== "finish") {
                setPendingDoneAfterWorking({ videoTransition: transition });
                if (state.targetState !== "working-exit") {
                    state.hasToolActivity = false;
                    setTargetState("working-exit");
                }
                return true;
            }

            const shouldRestart = state.activeBridgeAbstractState !== "done" || state.activeWorkingFamily !== "finish";
            return startWorkingAnimation(["finish"], {
                ...commandOptions,
                forceReselection: shouldRestart
            });
        }

        function consumeQueuedStateAfterWorking(defaultState = "idle-enter") {
            const nextQueuedState = normalizeText(state.queuedStateAfterWorking || defaultState || "idle-enter", "idle-enter");
            state.queuedStateAfterWorking = "";
            clearPendingDoneAfterWorking();
            clearWorkingSelection();
            emitStateChange("consumeQueuedStateAfterWorking");

            if (!nextQueuedState || nextQueuedState === getCurrentAgentIdleState() || nextQueuedState === "idle-enter") {
                return startIdleAnimation(state.activeIdlePoolKey || "default", {
                    forceReselection: true,
                    force: true,
                    skipWorkingRedirect: true
                });
            }

            return playStateNow(nextQueuedState);
        }

        function exitWorkingAnimation(nextState) {
            const fallbackState = normalizeText(nextState || getCurrentAgentIdleState() || defaultIdleState, defaultIdleState);
            const isWorkingActive = isWorkingFamilyPhaseState(state.currentState)
                || isWorkingFamilyPhaseState(state.targetState)
                || String(state.currentState || "").includes("working")
                || String(state.targetState || "").includes("working");

            if (!isWorkingActive) {
                logBridgeTrace("exitWorkingAnimation:skip-not-working", {
                    fallbackState
                });
                state.hasToolActivity = false;
                clearWorkingSelection();
                emitStateChange("exitWorkingAnimation:skip-not-working");
                if (fallbackState === getCurrentAgentIdleState()) {
                    return startIdleAnimation(state.activeIdlePoolKey || "default", {
                        forceReselection: true,
                        force: true,
                        skipWorkingRedirect: true
                    });
                }
                return setTargetState(fallbackState);
            }

            state.hasToolActivity = false;
            state.queuedStateAfterWorking = fallbackState;
            state.queuedWorkingFamily = "";

            if (state.currentState === "working-exit") {
                if (hasVideoPlaybackCompleted()) {
                    logBridgeTrace("exitWorkingAnimation:consume-completed-working-exit", {
                        fallbackState
                    });
                    return consumeQueuedStateAfterWorking(fallbackState);
                }

                logBridgeTrace("exitWorkingAnimation:awaiting-working-exit", {
                    fallbackState
                });
                emitStateChange("exitWorkingAnimation:awaiting-working-exit");
                return true;
            }

            if (state.targetState === "working-exit") {
                logBridgeTrace("exitWorkingAnimation:awaiting-working-exit", {
                    fallbackState
                });
                emitStateChange("exitWorkingAnimation:awaiting-working-exit");
                return true;
            }

            emitStateChange("exitWorkingAnimation:queue-exit");
            return setTargetState("working-exit");
        }

        function handleVideoEnded() {
            logAnimationTrace("videoEnded", {
                currentState: state.currentState,
                targetState: state.targetState,
                queuedStateAfterWorking: state.queuedStateAfterWorking,
                queuedWorkingFamily: state.queuedWorkingFamily,
                activeWorkingTransitionFromFamily: state.activeWorkingTransitionFromFamily,
                activeWorkingTransitionToFamily: state.activeWorkingTransitionToFamily,
                queuedStateAfterIdle: state.queuedStateAfterIdle,
                queuedIdleFamilyAfterExit: state.queuedIdleFamilyAfterExit
            });

            if (state.currentState === "idle-enter") {
                if (state.targetState === "idle-exit") {
                    return playStateNow("idle-exit");
                }

                if (state.idleLoopPlaysRemaining > 0) {
                    state.idleLoopPlaysRemaining -= 1;
                    emitStateChange("handleVideoEnded:idle-enter-loop");
                    return playStateNow("idle-loop");
                }

                return playStateNow("idle-exit");
            }

            if (state.currentState === "idle-loop") {
                if (state.targetState === "idle-exit") {
                    return playStateNow("idle-exit");
                }

                if (state.idleLoopPlaysRemaining > 0) {
                    state.idleLoopPlaysRemaining -= 1;
                    emitStateChange("handleVideoEnded:idle-loop-repeat");
                    return playStateNow("idle-loop");
                }

                return playStateNow("idle-exit");
            }

            if (state.currentState === "idle-exit") {
                const nextIdleFamily = normalizeText(state.queuedIdleFamilyAfterExit);
                const nextIdleState = normalizeText(state.queuedStateAfterIdle);
                state.queuedStateAfterIdle = "";
                state.queuedIdleFamilyAfterExit = "";

                if (nextIdleFamily) {
                    state.activeIdleFamily = nextIdleFamily;
                    state.idleLoopPlaysRemaining = pickIdleLoopRepeats();
                    emitStateChange("handleVideoEnded:idle-exit-next-family");
                    return playStateNow("idle-enter");
                }

                if (nextIdleState && nextIdleState !== "idle-enter") {
                    emitStateChange("handleVideoEnded:idle-exit-next-state");
                    return playStateNow(nextIdleState);
                }

                return startIdleAnimation(state.activeIdlePoolKey || "default", { forceReselection: true, force: true });
            }

            if (state.currentState === "speaking-enter") {
                if (state.targetState === "speaking-exit") {
                    return playStateNow("speaking-exit");
                }

                return playStateNow("speaking-loop");
            }

            if (state.currentState === "speaking-loop") {
                state.speakingLoopRepeats += 1;
                emitStateChange("handleVideoEnded:speaking-loop-repeat");
                if (state.targetState === "speaking-exit") {
                    return playStateNow("speaking-exit");
                }

                if (state.speakingLoopRepeats >= 6) {
                    state.speakingLoopRepeats = 0;
                    state.queuedStateAfterSpeaking = "speaking-enter";
                    emitStateChange("handleVideoEnded:speaking-loop-cap");
                    return playStateNow("speaking-exit");
                }

                return playStateNow("speaking-loop");
            }

            if (state.currentState === "speaking-exit") {
                const nextSpeakingState = normalizeText(state.queuedStateAfterSpeaking || "idle-enter", "idle-enter");
                state.queuedStateAfterSpeaking = "";
                state.speakingLoopRepeats = 0;
                emitStateChange("handleVideoEnded:speaking-exit");
                if (nextSpeakingState === "idle-enter") {
                    return startIdleAnimation(state.activeIdlePoolKey || "default", { forceReselection: true, force: true });
                }
                return playStateNow(nextSpeakingState);
            }

            if (state.currentState === "working-enter") {
                if (state.queuedWorkingFamily && state.queuedWorkingFamily !== state.activeWorkingFamily) {
                    const nextQueuedFamily = state.queuedWorkingFamily;
                    state.queuedWorkingFamily = "";
                    if (startWorkingFamilyTransition(nextQueuedFamily, { immediate: true })) {
                        emitStateChange("handleVideoEnded:working-enter-queued-family");
                        return true;
                    }
                }

                if (state.activeWorkingFamily === "finish") {
                    emitStateChange("handleVideoEnded:finish-enter-exit");
                    return playStateNow("working-exit");
                }

                if (state.targetState === "working-exit") {
                    return playStateNow("working-exit");
                }

                return playStateNow("working-loop");
            }

            if (state.currentState === "working-loop") {
                state.workingLoopRepeats += 1;
                emitStateChange("handleVideoEnded:working-loop-repeat");
                if (state.targetState === "working-exit" || state.queuedStateAfterWorking) {
                    return playStateNow("working-exit");
                }

                if (state.activeWorkingFamily === "finish") {
                    emitStateChange("handleVideoEnded:finish-loop-exit");
                    return playStateNow("working-exit");
                }

                if (state.queuedWorkingFamily && state.queuedWorkingFamily !== state.activeWorkingFamily) {
                    const nextQueuedFamily = state.queuedWorkingFamily;
                    if (startWorkingFamilyTransition(nextQueuedFamily, { immediate: true })) {
                        emitStateChange("handleVideoEnded:working-loop-switch-family");
                        return true;
                    }
                }

                return playStateNow("working-loop");
            }

            if (state.currentState === "working-transition") {
                const transitionToFamily = normalizeText(state.activeWorkingTransitionToFamily);
                const nextQueuedFamily = normalizeText(state.queuedWorkingFamily);
                if (transitionToFamily) {
                    state.activeWorkingFamily = transitionToFamily;
                }
                state.activeWorkingTransitionFromFamily = "";
                state.activeWorkingTransitionToFamily = "";
                state.queuedWorkingFamily = "";
                state.workingLoopRepeats = 0;
                emitStateChange("handleVideoEnded:working-transition");

                if (state.targetState === "working-exit" || state.queuedStateAfterWorking) {
                    return playStateNow("working-exit");
                }

                if (nextQueuedFamily && nextQueuedFamily !== state.activeWorkingFamily) {
                    state.queuedWorkingFamily = nextQueuedFamily;
                    if (startWorkingFamilyTransition(nextQueuedFamily, { immediate: true })) {
                        emitStateChange("handleVideoEnded:working-transition-next-family");
                        return true;
                    }
                }

                return playStateNow("working-loop");
            }

            if (state.currentState === "working-exit") {
                if (state.pendingDoneAfterWorking) {
                    return startDeferredFinishAfterWorking();
                }
                if (state.queuedWorkingFamily) {
                    state.activeWorkingFamily = state.queuedWorkingFamily;
                    state.queuedWorkingFamily = "";
                    state.queuedStateAfterWorking = "";
                    state.workingLoopRepeats = 0;
                    emitStateChange("handleVideoEnded:working-exit-resume");
                    return playStateNow("working-loop");
                }

                if (state.activeWorkingFamily === "finish") {
                    clearWorkingSelection();
                    emitStateChange("handleVideoEnded:finish-exit");
                    if (typeof helpers.onWorkingFinished === "function" && helpers.onWorkingFinished(getState()) === true) {
                        return true;
                    }
                    return startIdleAnimation("default", { forceReselection: true, force: true });
                }
                return consumeQueuedStateAfterWorking();
            }

            let nextState = getStateFlow()?.[state.currentState];
            const customIdleLoopStates = getCurrentAgentIdleLoopStates();
            const nextCandidates = Array.isArray(nextState) ? nextState : [nextState];
            const isWorkingExitState = String(state.currentState || "").includes("working")
                && nextCandidates.some((candidate) => isIdleLikeState(candidate));

            if (state.queuedStateAfterWorking && isWorkingExitState) {
                const nextQueuedState = state.queuedStateAfterWorking;
                state.queuedStateAfterWorking = "";
                if (String(state.currentState || "").includes("working")) {
                    clearWorkingSelection();
                }
                emitStateChange("handleVideoEnded:queued-working-exit");
                return playStateNow(nextQueuedState);
            }

            if (customIdleLoopStates && isIdleLikeState(state.currentState)) {
                nextState = customIdleLoopStates;
            } else if (getCurrentAgentIdleState() !== defaultIdleState && isIdleLikeState(state.currentState)) {
                nextState = getCurrentAgentIdleState();
            }

            if (nextState) {
                const next = Array.isArray(nextState)
                    ? nextState[Math.floor(random() * nextState.length)]
                    : nextState;
                if (String(state.currentState || "").includes("working") && !String(next || "").includes("working")) {
                    clearWorkingSelection();
                }
                emitStateChange("handleVideoEnded:generic-next");
                return playStateNow(next);
            }

            // 兜底：无论什么状态，都回到 idle 循环，不让动画停住
            logAnimationTrace("handleVideoEnded:fallback-to-idle", {
                currentState: state.currentState,
                targetState: state.targetState
            });
            return startIdleAnimation("default", { forceReselection: true, force: true });
        }

        emitStateChange("init");

        return {
            consumeQueuedStateAfterWorking,
            exitIdleAnimation,
            exitSpeakingAnimation,
            exitWorkingAnimation,
            getState,
            handleVideoEnded,
            playStateNow,
            resetIdleFamilySelection,
            resetRunState,
            resetWorkingFamilySelection,
            setActiveBridgeAbstractState,
            setTargetState,
            startErrorAnimation,
            startFinishAnimation,
            startIdleAnimation,
            startNotificationAnimation,
            startSpeakingAnimation,
            startThinkingAnimation,
            startToolRunningAnimation,
            startWaitingUserAnimation,
            startWorkingAnimation,
            syncCurrentState
        };
    }

    root.createPetAnimationController = createPetAnimationController;

    if (typeof module !== "undefined" && module.exports) {
        module.exports = {
            createPetAnimationController
        };
    }
})(typeof window !== "undefined" ? window : globalThis);
