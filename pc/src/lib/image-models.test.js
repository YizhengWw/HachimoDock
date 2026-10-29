import test from "node:test";
import assert from "node:assert/strict";
import { listImageModels, listImageModelCatalog, normalizeImageModels, imageModelOptions } from "./image-models.js";

test("image editing catalog filters incompatible, inactive and duplicate models", () => {
  const row = (id, input, output, extra = {}) => ({ id, modalities: { input_modalities: input, output_modalities: output }, ...extra });
  const models = normalizeImageModels([
    row("image-edit", ["text", "image"], ["image"]),
    row("text-only", ["text"], ["text"]),
    row("text-to-image", ["text"], ["image"]),
    row("video", ["text", "image"], ["video"]),
    row("disabled", ["image"], ["image"], { available: false }),
    row("retired", ["image"], ["image"], { lifecycle: { status: "Shutdown" } }),
    { id: "doubao-seedream-4-5-251128" }, { id: "doubao-seedream-4-5-251128" }, { id: "unknown-model" },
  ]);
  assert.deepEqual(models, ["doubao-seedream-4-5-251128", "image-edit"]);
});

test("shutdown models cannot return through presets or saved selections; retiring models remain", async () => {
  const shutdown = "doubao-seedream-4-0-250828";
  const retiring = "doubao-seedream-4-5-251128";
  const catalog = await listImageModelCatalog({ apiKey: "fixture", baseUrl: "https://ark.cn-beijing.volces.com" }, async () => Response.json({ data: [
    { id: shutdown, lifecycle: { status: "Shutdown" } },
    { id: retiring, lifecycle: { status: "Retiring" } },
    { id: "doubao-seedream-active", status: "Available" },
  ] }));
  assert.deepEqual(catalog.shutdownModels, [shutdown]);
  assert.ok(!catalog.models.includes(shutdown));
  assert.ok(catalog.models.includes(retiring));
  const options = imageModelOptions(catalog, shutdown);
  assert.ok(!options.includes(shutdown));
  assert.ok(options.includes(retiring));
  assert.ok(options.includes("doubao-seedream-active"));
});

test("catalog refresh only reads metadata and preserves authentication/cancellation errors", async () => {
  const config = { apiKey: "fixture-only", baseUrl: "https://ark.cn-beijing.volces.com" };
  const result = await listImageModels(config, async (url, init) => {
    assert.equal(url, "https://ark.cn-beijing.volces.com/api/v3/models");
    assert.equal(init.method, "GET");
    assert.equal(init.body, undefined);
    assert.equal(init.headers.Authorization, "Bearer fixture-only");
    return Response.json({ data: [{ id: "doubao-seedream-4-5-251128" }] });
  });
  assert.deepEqual(result, ["doubao-seedream-4-5-251128"]);
  await assert.rejects(listImageModels(config, async () => new Response("", { status: 401 })), /HTTP 401/);
  await assert.rejects(listImageModels(config, async () => Response.json({ data: [], has_more: true })), /不完整/);
  const controller = new AbortController();
  await assert.rejects(listImageModels({ ...config, signal: controller.signal }, async () => {
    controller.abort();
    return Response.json({ data: [] });
  }), { name: "AbortError" });
});
