import test from "node:test";
import assert from "node:assert/strict";
import { listVideoModels, normalizeVideoModels, requireAvailableVideoModel, selectDefaultVideoModel } from "./video-models.js";

test("empty selection defaults to the latest Fast version, then newest revision", () => {
  const models = normalizeVideoModels([
    { id: "doubao-seedance-1-0-pro-fast-260901" },
    { id: "doubao-seedance-2-0-fast-260128" },
    { id: "doubao-seedance-2-0-fast-260801" },
    { id: "doubao-seedance-3-0-pro-260901" },
    { id: "doubao-seedance-2-0-fast-260902", available: false },
  ]);
  const original = [...models];
  assert.equal(selectDefaultVideoModel(models), "doubao-seedance-2-0-fast-260801");
  assert.equal(selectDefaultVideoModel([...models].reverse()), "doubao-seedance-2-0-fast-260801");
  assert.deepEqual(models, original);
});

test("Fast ranking is numeric and supports dotted versions and dated releases", () => {
  const models = ["doubao-seedance-2.9-fast-260901", "doubao-seedance-2.10-fast-20260801",
    "doubao-seedance-2.10-fast-260831", "doubao-seedance-2.10-fast"]
    .map((id) => ({ id }));
  assert.equal(selectDefaultVideoModel(models), "doubao-seedance-2.10-fast-260831");
});

test("saved choices and manual endpoints are never replaced on catalog refresh", () => {
  const models = [{ id: "doubao-seedance-2-0-fast-260128" }];
  for (const chosen of ["ep-test-only", "doubao-seedance-1-0-pro-fast-250610", "custom-model"]) {
    assert.equal(selectDefaultVideoModel(models, chosen), chosen);
  }
  assert.equal(selectDefaultVideoModel(models, "", true), "");
});

test("no Fast result does not silently select a regular model", () => {
  assert.equal(selectDefaultVideoModel([]), "");
  assert.equal(selectDefaultVideoModel([{ id: "doubao-seedance-2-0-mini" },
    { id: "doubao-seedance-2-0-fastest" }]), "");
});

test("a manually configured endpoint is not blocked by catalog availability", async () => {
  await requireAvailableVideoModel({ provider: "volcengine", apiKey: "test-only", model: "ep-test-only" });
  await assert.rejects(requireAvailableVideoModel({ provider: "volcengine", apiKey: "test-only", model: "" }), /填写/);
});

test("catalog tolerates omitted modality metadata while excluding explicit retirement", () => {
  assert.deepEqual(normalizeVideoModels([
    { id: "doubao-seedance-current" },
    { id: "doubao-seedance-retired", lifecycle: { status: "Retiring" } },
  ]), [{ id: "doubao-seedance-current", label: "doubao-seedance-current" }]);
});

const model = (id, patch = {}) => ({ id, modalities: { input_modalities: ["text", "image"], output_modalities: ["video"] }, ...patch });
test("catalog drops retiring, shutdown, unavailable and incompatible models", () => {
  const result = normalizeVideoModels([
    model("doubao-seedance-1-5-pro", { status: "Retiring" }),
    model("doubao-seedance-old", { status: "Shutdown" }),
    model("doubao-seedance-disabled", { available: false }),
    model("doubao-seedance-text-only", { modalities: { input_modalities: ["text"], output_modalities: ["video"] } }),
    model("doubao-seedream-image"),
    model("doubao-seedance-live"), model("doubao-seedance-live"),
  ]);
  assert.deepEqual(result.map((m) => m.id), ["doubao-seedance-live"]);
});
test("model discovery uses only API Key and never submits a generation job", async () => {
  let calls = 0;
  const result = await listVideoModels({ apiKey: "test-only-key" }, async (url, init) => {
    calls += 1;
    assert.equal(url, "https://ark.cn-beijing.volces.com/api/v3/models");
    assert.equal(init.method, "GET");
    assert.equal(init.headers.Authorization, "Bearer test-only-key");
    assert.equal(init.body, undefined);
    return Response.json({ data: [model("doubao-seedance-live")] });
  });
  assert.equal(calls, 1);
  assert.equal(result.length, 1);
});
test("discovery fails closed for authentication and malformed responses", async () => {
  await assert.rejects(listVideoModels({ apiKey: "test-only-key" }, async () => new Response("", { status: 401 })), /鉴权/);
  await assert.rejects(listVideoModels({ apiKey: "test-only-key" }, async () => Response.json({})), /完整/);
  await assert.rejects(listVideoModels({}), /API Key/);
});
test("a cancelled request cannot publish an old credentials catalog", async () => {
  const controller = new AbortController();
  await assert.rejects(listVideoModels({ apiKey: "test", signal: controller.signal }, async () => {
    controller.abort();
    return Response.json({ data: [model("doubao-seedance-live")] });
  }), { name: "AbortError" });
});
