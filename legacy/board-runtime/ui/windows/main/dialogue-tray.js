(function attachPetClawDialogueTray(root) {
    function resolveDialogueVariant(sender, status) {
        const senderText = String(sender || "");
        const statusText = String(status || "");

        if (senderText.includes("系统") || statusText.includes("异常") || statusText.includes("失败")) {
            return "system";
        }
        if (senderText.includes("你") || senderText.includes("用户")) {
            return "user";
        }
        return "agent";
    }

    function splitDialogueText(text) {
        const sentences = String(text || "").split(/([，。,.])/g);
        const merged = [];

        for (let index = 0; index < sentences.length; index += 2) {
            const content = sentences[index];
            const punct = sentences[index + 1] || "";
            if (content && content.trim()) {
                merged.push((content + punct).trim());
            }
        }

        return merged;
    }

    function normalizeDialogueSegments(textOrSegments) {
        if (Array.isArray(textOrSegments)) {
            return textOrSegments.filter((item) => item && item.trim());
        }
        return splitDialogueText(textOrSegments);
    }

    function estimateDialogueSegmentDuration(text) {
        const length = Array.from(String(text || "").trim()).length;
        const typingDuration = Math.max(520, Math.min(length * 42, 2100));
        const holdDuration = Math.max(1500, Math.min(length * 105 + 900, 3600));
        return typingDuration + holdDuration + 260;
    }

    function createPetClawDialogueTray(options = {}) {
        const elements = {
            stage: options.stage || null,
            panel: options.panel || null,
            sender: options.sender || null,
            status: options.status || null,
            text: options.text || null,
            note: options.note || null
        };
        const getRuntimeAgentLabel = typeof options.getRuntimeAgentLabel === "function"
            ? options.getRuntimeAgentLabel
            : () => "";
        const getCurrentView = typeof options.getCurrentView === "function"
            ? options.getCurrentView
            : () => "";
        const homeView = String(options.homeView || "home");
        const idleStatus = String(options.idleStatus || "OpenClaw");

        const state = {
            queue: [],
            active: false,
            typeTimer: null,
            advanceTimer: null,
            hideTimer: null
        };

        function clearTimers() {
            root.clearInterval(state.typeTimer);
            root.clearTimeout(state.advanceTimer);
            root.clearTimeout(state.hideTimer);
            state.typeTimer = null;
            state.advanceTimer = null;
            state.hideTimer = null;
        }

        function setVisible(visible) {
            if (!elements.stage) return;
            elements.stage.classList.toggle("is-visible", Boolean(visible));
        }

        function renderIdle() {
            if (!elements.panel || state.active || state.queue.length || getCurrentView() !== homeView) {
                return;
            }

            elements.panel.dataset.variant = "agent";
            elements.panel.classList.add("is-idle");
            elements.panel.classList.add("visible");
            elements.panel.classList.remove("hiding");
            setVisible(true);

            if (elements.sender) {
                elements.sender.textContent = getRuntimeAgentLabel();
            }
            if (elements.status) {
                elements.status.textContent = idleStatus;
            }
            if (elements.text) {
                elements.text.classList.remove("typing");
                elements.text.textContent = "";
            }
            if (elements.note) {
                elements.note.textContent = "";
            }
        }

        function finishDialogueSegment() {
            if (!elements.panel) {
                state.active = false;
                return;
            }

            if (state.queue.length) {
                state.active = false;
                playDialogueQueue();
                return;
            }

            elements.panel.classList.add("hiding");
            elements.panel.classList.remove("visible");
            state.hideTimer = root.setTimeout(() => {
                elements.panel.classList.remove("hiding");
                state.active = false;
                elements.panel.dataset.variant = "agent";
                if (elements.text) {
                    elements.text.classList.remove("typing");
                    elements.text.textContent = "";
                }
                if (elements.note) {
                    elements.note.textContent = "";
                }

                if (getCurrentView() === homeView) {
                    renderIdle();
                } else {
                    elements.panel.classList.remove("visible");
                    setVisible(false);
                }
            }, 360);
        }

        function playDialogueQueue() {
            if (state.active || !state.queue.length || !elements.panel) return;

            setVisible(true);
            state.active = true;

            const current = state.queue.shift();
            const characters = Array.from(current.text);
            const totalTypingDuration = Math.max(520, Math.min(characters.length * 42, 2100));
            const typingInterval = characters.length
                ? Math.max(24, Math.min(72, Math.round(totalTypingDuration / characters.length)))
                : 32;

            clearTimers();
            elements.panel.dataset.variant = resolveDialogueVariant(current.sender, current.status);
            elements.panel.classList.remove("is-idle");
            elements.panel.classList.remove("hiding");
            elements.panel.classList.add("visible");

            if (elements.sender) {
                elements.sender.textContent = current.sender;
            }
            if (elements.status) {
                elements.status.textContent = current.status;
            }
            if (elements.text) {
                elements.text.textContent = "";
                elements.text.classList.add("typing");
            }

            if (!characters.length) {
                if (elements.text) {
                    elements.text.classList.remove("typing");
                }
                finishDialogueSegment();
                return;
            }

            let index = 0;
            state.typeTimer = root.setInterval(() => {
                index += 1;
                if (elements.text) {
                    elements.text.textContent = characters.slice(0, index).join("");
                }

                if (index >= characters.length) {
                    root.clearInterval(state.typeTimer);
                    state.typeTimer = null;
                    if (elements.text) {
                        elements.text.classList.remove("typing");
                    }

                    const holdDuration = Math.max(1500, Math.min(characters.length * 105 + 900, 3600));
                    state.advanceTimer = root.setTimeout(() => {
                        finishDialogueSegment();
                    }, holdDuration);
                }
            }, typingInterval);
        }

        function estimateDuration(textOrSegments) {
            return normalizeDialogueSegments(textOrSegments).reduce((sum, segment) => {
                return sum + estimateDialogueSegmentDuration(segment);
            }, 380);
        }

        function enqueue(textOrSegments, enqueueOptions = {}) {
            const segments = normalizeDialogueSegments(textOrSegments);
            if (!segments.length) return 0;

            const sender = String(enqueueOptions.sender || getRuntimeAgentLabel()).trim() || getRuntimeAgentLabel();
            const status = String(enqueueOptions.status || "对话中").trim() || "对话中";

            for (const segment of segments) {
                state.queue.push({
                    text: segment,
                    sender,
                    status
                });
            }

            setVisible(true);
            playDialogueQueue();
            return estimateDuration(segments);
        }

        function clear(clearOptions = {}) {
            const keepIdle = clearOptions.keepIdle !== false;

            clearTimers();
            state.queue = [];
            state.active = false;

            if (!elements.panel) return;

            elements.panel.classList.remove("hiding");
            elements.panel.classList.remove("visible");
            elements.panel.classList.remove("is-idle");
            elements.panel.dataset.variant = "agent";

            if (elements.text) {
                elements.text.classList.remove("typing");
                elements.text.textContent = "";
            }
            if (elements.note) {
                elements.note.textContent = "";
            }

            if (keepIdle && getCurrentView() === homeView) {
                renderIdle();
                return;
            }

            setVisible(false);
        }

        return {
            clear,
            clearTimers,
            enqueue,
            estimateDuration,
            isBusy() {
                return state.active || state.queue.length > 0;
            },
            renderIdle,
            setVisible
        };
    }

    root.createPetClawDialogueTray = createPetClawDialogueTray;
})(typeof window !== "undefined" ? window : globalThis);

if (typeof module !== "undefined" && module.exports) {
    module.exports = {
        createPetClawDialogueTray: globalThis.createPetClawDialogueTray
    };
}
