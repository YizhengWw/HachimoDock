(function attachPetAnimationConfig(root) {
    const STATE_FLOW = {};

    const GENERATED_VIDEO_ROOT = "pets/terrier/generated-videos";

    function generatedRawVideo(family, phase) {
        return `${GENERATED_VIDEO_ROOT}/${family}/${family}.${phase}.raw.mp4`;
    }

    const TERRIER_STATE_ASSETS = {};

    const TERRIER_IDLE_FAMILIES = {
        begging: {
            enter: generatedRawVideo("idle.begging", "enter"),
            loop: generatedRawVideo("idle.begging", "loop"),
            exit: generatedRawVideo("idle.begging", "exit")
        },
        daydreaming: {
            enter: generatedRawVideo("idle.daydreaming", "enter"),
            loop: generatedRawVideo("idle.daydreaming", "loop"),
            exit: generatedRawVideo("idle.daydreaming", "exit")
        },
        eating: {
            enter: generatedRawVideo("idle.eating", "enter"),
            loop: generatedRawVideo("idle.eating", "loop"),
            exit: generatedRawVideo("idle.eating", "exit")
        },
        playing: {
            enter: generatedRawVideo("idle.playing", "enter"),
            loop: generatedRawVideo("idle.playing", "loop"),
            exit: generatedRawVideo("idle.playing", "exit")
        },
        reading: {
            enter: generatedRawVideo("idle.reading", "enter"),
            loop: generatedRawVideo("idle.reading", "loop"),
            exit: generatedRawVideo("idle.reading", "exit")
        },
        traveling: {
            enter: generatedRawVideo("idle.traveling", "enter"),
            loop: generatedRawVideo("idle.traveling", "loop"),
            exit: generatedRawVideo("idle.traveling", "exit")
        },
        wandering: {
            enter: generatedRawVideo("idle.wandering", "enter"),
            loop: generatedRawVideo("idle.wandering", "loop"),
            exit: generatedRawVideo("idle.wandering", "exit")
        },
        daze: {
            enter: generatedRawVideo("idle.daydreaming", "enter"),
            loop: generatedRawVideo("idle.daydreaming", "loop"),
            exit: generatedRawVideo("idle.daydreaming", "exit")
        }
    };

    const TERRIER_IDLE_POOLS = {
        default: [
            "begging",
            "daydreaming",
            "eating",
            "playing",
            "reading",
            "traveling",
            "wandering"
        ],
        focused: ["reading", "daydreaming"],
        low_energy: ["daydreaming"],
        fallback: ["daydreaming"]
    };

    const TERRIER_SPEAKING_ASSETS = {
        enter: generatedRawVideo("welcome", "enter"),
        loop: generatedRawVideo("idle.wandering", "loop"),
        exit: generatedRawVideo("welcome", "exit")
    };

    const TERRIER_WORKING_FAMILIES = {
        decide: {
            enter: generatedRawVideo("waiting_user", "enter"),
            loop: generatedRawVideo("waiting_user", "loop"),
            exit: generatedRawVideo("waiting_user", "exit")
        },
        notification: {
            enter: generatedRawVideo("waiting_user", "enter"),
            loop: generatedRawVideo("waiting_user", "loop"),
            exit: generatedRawVideo("waiting_user", "exit")
        },
        error: {
            enter: generatedRawVideo("error", "enter"),
            loop: generatedRawVideo("error", "loop"),
            exit: generatedRawVideo("error", "exit")
        },
        finish: {
            enter: generatedRawVideo("done", "enter"),
            loop: generatedRawVideo("done", "loop"),
            exit: generatedRawVideo("done", "exit")
        },
        thinking: {
            enter: generatedRawVideo("working.thinking", "enter"),
            loop: generatedRawVideo("working.thinking", "loop"),
            exit: generatedRawVideo("working.thinking", "exit")
        },
        typing: {
            enter: generatedRawVideo("working.typing", "enter"),
            loop: generatedRawVideo("working.typing", "loop"),
            exit: generatedRawVideo("working.typing", "exit")
        },
        browsing: {
            enter: generatedRawVideo("working.browsing", "enter"),
            loop: generatedRawVideo("working.browsing", "loop"),
            exit: generatedRawVideo("working.browsing", "exit")
        }
    };

    const TERRIER_WORKING_TRANSITIONS = {};

    const MOCK_AGENTS = [
        {
            id: "ops",
            name: "梗犬小助手",
            idleState: "idle-enter",
            idleLoopStates: null,
            stateFlow: {},
            stateAssets: TERRIER_STATE_ASSETS,
            idleFamilies: TERRIER_IDLE_FAMILIES,
            idlePools: TERRIER_IDLE_POOLS,
            speakingAssets: TERRIER_SPEAKING_ASSETS,
            workingFamilies: TERRIER_WORKING_FAMILIES,
            workingTransitions: TERRIER_WORKING_TRANSITIONS
        }
    ];

    const PET_ANIMATION_CONFIG = Object.freeze({
        GENERATED_VIDEO_ROOT,
        STATE_FLOW,
        TERRIER_STATE_ASSETS,
        TERRIER_IDLE_FAMILIES,
        TERRIER_IDLE_POOLS,
        TERRIER_SPEAKING_ASSETS,
        TERRIER_WORKING_FAMILIES,
        TERRIER_WORKING_TRANSITIONS,
        MOCK_AGENTS
    });

    root.PET_ANIMATION_CONFIG = PET_ANIMATION_CONFIG;

    if (typeof module !== "undefined" && module.exports) {
        module.exports = PET_ANIMATION_CONFIG;
    }
})(typeof window !== "undefined" ? window : globalThis);
