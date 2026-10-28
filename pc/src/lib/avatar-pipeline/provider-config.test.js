/**
 * [Input] Avatar provider-config persistence helper source, Ark image-model default, and API-key precedence resolver.
 * [Output] Node regression coverage for the shared localStorage contract, Seedream default, and user-config-only credentials.
 * [Pos] test node in pc/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { loadArkImageConfig, loadProviderConfig, saveProviderConfig, resolveProviderApiKey, initializeProviderConfigs } from "./provider-config.js";

const libDir = dirname(fileURLToPath(import.meta.url));

test("public build has no default keys and preserves legacy user credentials", async () => {
  const previous=globalThis.window;
  try {
    globalThis.window={__TAURI_INTERNALS__:{}};
    const empty={getItem:()=>null,removeItem:()=>{}};
    await initializeProviderConfigs(empty,async()=>({}));
    assert.equal(loadProviderConfig("volcengine",empty).apiKey,"");
    assert.equal(loadProviderConfig("custom",empty).apiKey,"");
    let saved=false,removed=false;
    const legacy={getItem:key=>key.endsWith("volcengine")?JSON.stringify({apiKey:"user-owned-fixture",model:"ep-user"}):null,removeItem:()=>{assert(saved);removed=true;}};
    await initializeProviderConfigs(legacy,async(command,args)=>{
      if(command==="load_generation_provider_configs")return {};
      assert.equal(args.config.apiKey,"user-owned-fixture");saved=true;
    });
    assert(saved&&removed);
    assert.equal(loadProviderConfig("volcengine",legacy).apiKey,"user-owned-fixture");
  } finally {if(previous===undefined)delete globalThis.window;else globalThis.window=previous;}
});

test("saved video model and prompt model roundtrip independently", () => {
  let saved;
  const storage = { getItem: () => saved, setItem: (_key, value) => { saved = value; } };
  saveProviderConfig("volcengine", { apiKey: "test-key", model: "ep-video-choice", thinkingModel: "ep-prompt-choice", fastGeneration: true }, storage);
  const config = loadProviderConfig("volcengine", storage);
  assert.equal(config.model, "ep-video-choice");
  assert.equal(config.thinkingModel, "ep-prompt-choice");
  saveProviderConfig("volcengine", { ...config, model: "ep-video-second" }, storage);
  assert.equal(loadProviderConfig("volcengine", storage).thinkingModel, "ep-prompt-choice");
  assert.equal(loadProviderConfig("volcengine", storage).model, "ep-video-second");
});

test("previous forced Pro default migrates to Lite without losing keys; selected Pro or endpoint remains", () => {
  for (const thinkingModel of ["", "doubao-seed-2-0-pro-260215", "doubao-seed-2-1-pro-260915", "ep-user-choice"]) {
    const config = loadProviderConfig("volcengine", { getItem: () => JSON.stringify({ apiKey: "test-keep-key", thinkingModel }) });
    assert.equal(config.apiKey, "test-keep-key");
    assert.equal(config.thinkingModel, thinkingModel && thinkingModel !== "doubao-seed-2-0-pro-260215" ? thinkingModel : "doubao-seed-2-1-lite-260915");
  }
});

test("failed or unavailable storage never reports a successful save or exposes credentials", () => {
  assert.throws(() => saveProviderConfig("custom", {}, null), /配置未保存/);
  assert.throws(() => saveProviderConfig("custom", { apiKey: "test-secret" }, {
    setItem() { throw new Error("test-secret: quota exceeded"); },
  }), error => error.message.includes("配置未保存") && !error.message.includes("test-secret"));
});

test("generation parameters persist without overriding credentials or identity", () => {
  let saved;
  const storage = { getItem: () => saved, setItem: (_key, value) => { saved = value; } };
  const videoParameters = { duration: 10, resolution: "720p", seed: 42, cameraFixed: true, apiKey: "ignored", model: "ignored" };
  saveProviderConfig("volcengine", { apiKey: "user-test-key", videoParameters }, storage);
  const config = loadProviderConfig("volcengine", storage);
  assert.deepEqual(config.videoParameters, { duration: 10, resolution: "720p", seed: 42, cameraFixed: true });
  assert.equal(config.apiKey, "user-test-key");
});

test("provider config helper owns the shared localStorage key and Volcengine defaults", () => {
  const source = readFileSync(join(libDir, "provider-config.js"), "utf8");

  assert.match(source, /export const STORAGE_KEY_PREFIX = "claw-pet\.video-gen-config\."/);
  assert.match(source, /export const DEFAULT_PROVIDER_ID = "volcengine"/);
  assert.match(source, /export function loadProviderConfig/);
  assert.match(source, /export function saveProviderConfig/);
  assert.match(source, /DEFAULT_VOLCANO_BASE_URL/);
  assert.match(source, /DEFAULT_THINKING_MODEL/);
  assert.match(source, /DEFAULT_VOLCANO_IMAGE_MODEL/);
  assert.match(source, /VOLCENGINE_IMAGE_MODEL/);
  assert.match(source, /fastGeneration: saved\.fastGeneration !== false/);
  assert.doesNotMatch(source, /__PET_MANAGER_INTERNAL_CONTENT_API_KEY__/);
});

test("provider API key requires an explicit user value", () => {
  assert.equal(resolveProviderApiKey("volcengine", "user-key", "unused-fallback"), "user-key");
  assert.equal(resolveProviderApiKey("volcengine", "", "unused-fallback"), "");
  assert.equal(resolveProviderApiKey("custom", "", "unused-fallback"), "");
});

test("Ark image editing resolves independently from the selected video provider", () => {
  const storage = {
    getItem(key) {
      return key.endsWith("volcengine")
        ? JSON.stringify({ apiKey: "ark-image-key", fastGeneration: true })
        : null;
    },
  };

  assert.deepEqual(loadArkImageConfig(storage), {
    apiKey: "ark-image-key",
    baseUrl: "https://ark.cn-beijing.volces.com",
    model: "doubao-seedream-5-0-flash-260915",
  });
});

test("saved image model is preserved independently of video, prompt and key", () => {
  const storage = { getItem: () => JSON.stringify({
    apiKey: "preserved-test-key", imageModel: "doubao-seedream-5-0-lite-260128",
    model: "doubao-seedance-2-0-mini-260615", thinkingModel: "doubao-seed-2-1-pro-260915", fastGeneration: true,
  }) };
  const config = loadProviderConfig("volcengine", storage);
  assert.equal(config.imageModel, "doubao-seedream-5-0-lite-260128");
  assert.equal(loadArkImageConfig(storage).model, "doubao-seedream-5-0-lite-260128");
  assert.equal(config.apiKey, "preserved-test-key");
  assert.equal(config.model, "doubao-seedance-2-0-mini-260615");
  assert.equal(config.thinkingModel, "doubao-seed-2-1-pro-260915");
});

test("blank image choices use the default; configured endpoints survive browser reload", async () => {
  let saved;
  const storage = { getItem: () => saved, setItem: (_key, value) => { saved = value; } };
  for (const imageModel of [undefined, "", "   "]) {
    saveProviderConfig("volcengine", { apiKey: "fixture", imageModel }, storage);
    assert.equal(loadArkImageConfig(storage).model, "doubao-seedream-5-0-flash-260915");
  }
  saveProviderConfig("volcengine", { apiKey: "fixture", imageModel: "  ep-chosen-image  " }, storage);
  assert.equal(loadProviderConfig("volcengine", storage).imageModel, "ep-chosen-image");
  assert.equal(loadArkImageConfig(storage).model, "ep-chosen-image");
});

test("native credential storage roundtrips the configured image model", async () => {
  const previous = globalThis.window;
  let stored = {};
  globalThis.window = { __TAURI_INTERNALS__: { invoke: async (command, args) => {
    if (command === "load_generation_provider_configs") return structuredClone(stored);
    assert.equal(command, "save_generation_provider_config");
    stored[args.providerId] = structuredClone(args.config);
  } } };
  try {
    const storage = { getItem: () => null };
    await initializeProviderConfigs(storage);
    await saveProviderConfig("volcengine", { apiKey: "fixture", imageModel: "ep-native-image" }, storage);
    await initializeProviderConfigs(storage);
    assert.equal(loadArkImageConfig(storage).model, "ep-native-image");
    assert.equal(loadArkImageConfig(storage).apiKey, "fixture");
  } finally {
    if (previous === undefined) delete globalThis.window;
    else globalThis.window = previous;
  }
});
