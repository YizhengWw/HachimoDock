/** Offline state-machine regression. Storage is injected; no generation or user data. */
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";

function deferred() {
  let resolve, reject;
  const promise = new Promise((ok, fail) => { resolve = ok; reject = fail; });
  return { promise, resolve, reject };
}
function harness(overrides = {}) {
  const source = readFileSync(new URL("./generation-task.js", import.meta.url), "utf8")
    .replace(/^import .*;\n/gm, "").replace(/^export /gm, "");
  const context = vm.createContext({ AbortController, Date, crypto: globalThis.crypto,
    clearTimeout, setTimeout, console, recordDiagnostic() {},
    CUSTOM_GENERATION_FAMILIES: [{ family: "welcome" }],
    loadGenerationSession: async () => ({ id: "old", config: { provider: "volcengine" } }),
    saveGenerationSession: async () => {}, beginGenerationSession: () => new Promise(() => {}),
    publicGenerationConfig: () => ({}), ...overrides });
  vm.runInContext(source + "\nthis.api={startGenerationTask,dismissGenerationSession,resumeGenerationTask,getGenerationTask};", context);
  return context.api;
}
const input = { appearanceName: "new", providerConfig: { provider: "volcengine" } };

test("new generation cannot overtake a pending dismissal read", async () => {
  const read = deferred(), api = harness({ loadGenerationSession: () => read.promise });
  const dismissal = api.dismissGenerationSession();
  assert.throws(() => api.startGenerationTask(input), /任务记录/);
  read.resolve({ id: "old" }); await dismissal;
  api.startGenerationTask(input);
  assert.equal(api.getGenerationTask().status, "running");
});

test("new generation cannot overtake a pending dismissal write", async () => {
  const write = deferred(), entered = deferred();
  const api = harness({ saveGenerationSession: () => { entered.resolve(); return write.promise; } });
  const dismissal = api.dismissGenerationSession(); await entered.promise;
  assert.throws(() => api.startGenerationTask(input), /任务记录/);
  write.resolve(); await dismissal;
  api.startGenerationTask(input);
  assert.equal(api.getGenerationTask().status, "running");
});

test("failed dismissal releases the transition without hiding the failure", async () => {
  const api = harness({ saveGenerationSession: async () => { throw Error("storage failure"); } });
  await assert.rejects(api.dismissGenerationSession(), /storage failure/);
  api.startGenerationTask(input);
  assert.equal(api.getGenerationTask().status, "running");
});

test("dismissal cannot race a resume loading its checkpoint", async () => {
  const read = deferred(); let reads = 0;
  const api = harness({ loadGenerationSession: () => { reads++; return read.promise; } });
  const resume = api.resumeGenerationTask();
  const dismissal = api.dismissGenerationSession();
  const observedReads = reads;
  read.resolve(null); await Promise.all([resume, dismissal]);
  assert.equal(observedReads, 1);
  api.startGenerationTask(input);
  assert.equal(api.getGenerationTask().status, "running");
});

test("resume releases its reservation before starting the restored task", async () => {
  const api = harness({loadGenerationSession: async () => ({id: "old", config: {provider: "volcengine"}, skipProcessing:true}),
    saveGenerationSession: () => new Promise(() => {}),
    loadProviderConfig: () => ({}), loadArkImageConfig: () => ({}), resumeGenerationImageEdit: () => ({})});
  await api.resumeGenerationTask();
  assert.equal(api.getGenerationTask().status, "running");
});
