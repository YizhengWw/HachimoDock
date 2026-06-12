const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const repoRoot = path.resolve(__dirname, "..");
const config = require("../ui/windows/main/pet-animation-config.js");
const { createPetAnimationController } = require("../ui/windows/main/pet-animation-controller.js");
const {
  resolvePetLifecycleAction,
  shouldRetainWorkingThroughIdle,
} = require("../ui/windows/main/pet-lifecycle-resolver.js");

function isIdleFamilyPhaseState(state) {
  return ["idle-enter", "idle-loop", "idle-exit"].includes(String(state || "").trim());
}

function isWorkingFamilyPhaseState(state) {
  return ["working-enter", "working-loop", "working-exit", "working-transition"].includes(String(state || "").trim());
}

function collectLogicalAssetPaths(value, output = new Set()) {
  if (typeof value === "string") {
    const normalized = value.trim();
    if (normalized.startsWith("pets/")) {
      output.add(normalized);
    }
    return output;
  }

  if (Array.isArray(value)) {
    value.forEach((item) => collectLogicalAssetPaths(item, output));
    return output;
  }

  if (value && typeof value === "object") {
    Object.values(value).forEach((item) => collectLogicalAssetPaths(item, output));
  }

  return output;
}

const [agent] = config.MOCK_AGENTS;
assert(agent, "expected at least one configured agent");

assert.equal(config.GENERATED_VIDEO_ROOT, "pets/terrier/generated-videos");
assert(agent.workingFamilies.thinking, "working.thinking family is required");
assert(agent.workingFamilies.typing, "working.typing family is required");
assert(agent.workingFamilies.browsing, "working.browsing family is required");
assert(agent.workingFamilies.decide, "working.decide family remains a separate waiting-user state");
assert.equal(agent.workingFamilies.write, undefined, "working.write should be exposed as working.typing");
assert.deepEqual(agent.workingTransitions, {}, "generic working variants should not be fixed by transition mapping");

const playEvents = [];
const selectedWorkingFamilies = [];
const controller = createPetAnimationController({
  helpers: {
    isIdleFamilyPhaseState,
    isSpeakingPhaseState: () => false,
    isWorkingFamilyPhaseState,
    pickWorkingFamily(preferredNames = null) {
      const names = Array.isArray(preferredNames) && preferredNames.length
        ? preferredNames
        : Object.keys(agent.workingFamilies);
      const selected = names.find((name) => agent.workingFamilies[name]) || "";
      selectedWorkingFamilies.push({ preferredNames, selected });
      return selected;
    },
    resolvePlannedVideoSource: () => "",
  },
  onPlay(state, snapshot) {
    playEvents.push({ state, snapshot });
  },
});

controller.startWorkingAnimation(null, { forceReselection: true });
const toolRunningState = controller.getState();
assert(["thinking", "typing", "browsing"].includes(toolRunningState.activeWorkingFamily));
assert.deepEqual(playEvents.map((event) => event.state), ["working-enter"]);
assert.deepEqual(selectedWorkingFamilies[0].preferredNames, ["thinking", "typing", "browsing"]);

assert.deepEqual(
  resolvePetLifecycleAction({ state: "thinking" }),
  {
    abstractState: "working",
    command: { type: "startWorking" },
    reason: "thinking",
  },
);

assert.deepEqual(
  resolvePetLifecycleAction({ state: "tool_running", toolName: "Browser" }),
  {
    abstractState: "working",
    command: { type: "startWorking" },
    reason: "tool_running",
  },
);

assert.deepEqual(
  resolvePetLifecycleAction({ state: "waiting_user" }),
  {
    abstractState: "waiting_user",
    command: { type: "startWaitingUser" },
    reason: "waiting_user",
  },
);

assert.equal(
  shouldRetainWorkingThroughIdle({
    actionAbstractState: "idle.default",
    activeBridgeAbstractState: "done",
    isWorkingAnimationActive: true,
    lastWorkingAtMs: 10_000,
    nowMs: 12_999,
  }),
  true,
  "idle/rest should be buffered when the next working burst lands within 3s",
);

assert.equal(
  shouldRetainWorkingThroughIdle({
    actionAbstractState: "idle.default",
    activeBridgeAbstractState: "working",
    isWorkingAnimationActive: true,
    lastWorkingAtMs: 10_000,
    nowMs: 13_001,
  }),
  false,
  "idle/rest should be allowed after the 3s working buffer expires",
);

const logicalAssetPaths = collectLogicalAssetPaths(agent);
for (const family of ["thinking", "typing", "browsing"]) {
  for (const phase of ["enter", "loop", "exit"]) {
    assert(
      logicalAssetPaths.has(`pets/terrier/generated-videos/working.${family}/working.${family}.${phase}.raw.mp4`),
      `working.${family}.${phase} asset should be configured`,
    );
  }
}

for (const logicalPath of logicalAssetPaths) {
  assert(
    !logicalPath.includes("/thinking/") && !logicalPath.includes("/tool_running."),
    `working assets should use working.* names: ${logicalPath}`,
  );
  assert(
    fs.existsSync(path.join(repoRoot, "assets", logicalPath)),
    `configured asset is missing: ${logicalPath}`,
  );
}

assert(
  !fs.existsSync(path.join(repoRoot, "assets/pets/devon")),
  "retired devon assets should be removed after switching to terrier clips",
);
