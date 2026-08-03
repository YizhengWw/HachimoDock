/**
 * [Input] The pinned IMG.LY ESM bundle, CSP-safe ndarray compatibility layer, and Vite transform.
 * [Output] Regression coverage for ndarray behavior and removal of dynamic JavaScript execution.
 * [Pos] Node test guarding release background-removal compatibility under the strict Tauri CSP.
 * [Sync] If this file changes, update ndarray-csp.js, csp-safe-background-removal.js, and ref/src/.folder.md.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import dynamicNdarray from "ndarray";
import cspNdarray from "./ndarray-csp.js";
import { cspSafeBackgroundRemovalPlugin } from "../../csp-safe-background-removal.js";

const libDir = dirname(fileURLToPath(import.meta.url));
const refRoot = resolve(libDir, "../..");
const imglyEntry = resolve(
  refRoot,
  "node_modules/@imgly/background-removal/dist/index.mjs",
);
const ortEntries = [
  "node_modules/onnxruntime-web/dist/ort.bundle.min.mjs",
  "node_modules/onnxruntime-web/dist/ort.webgpu.bundle.min.mjs",
].map((path) => resolve(refRoot, path));

function metadata(view) {
  return {
    dtype: view.dtype,
    dimension: view.dimension,
    shape: [...view.shape],
    stride: [...view.stride],
    offset: view.offset,
    size: view.size,
    order: [...view.order],
  };
}

function assertEquivalent(left, right) {
  assert.deepEqual(metadata(left), metadata(right));
}

test("CSP-safe ndarray matches ndarray 1.0.19 view operations", () => {
  const source = Uint8Array.from({ length: 24 }, (_, index) => index);
  const dynamic = dynamicNdarray(source.slice(), [2, 3, 4]);
  const safe = cspNdarray(source.slice(), [2, 3, 4]);

  assertEquivalent(safe, dynamic);
  assert.equal(safe.get(1, 2, 3), dynamic.get(1, 2, 3));
  assert.equal(safe.set(1, 1, 2, 201), dynamic.set(1, 1, 2, 201));
  assert.equal(safe.get(1, 1, 2), dynamic.get(1, 1, 2));

  const operations = [
    (view) => view.hi(2, 2, 3),
    (view) => view.lo(0, 1, 1),
    (view) => view.step(-1, 2, 1),
    (view) => view.transpose(2, 0, 1),
    (view) => view.pick(1, undefined, 2),
  ];
  for (const operation of operations) {
    assertEquivalent(operation(safe), operation(dynamic));
  }
});

test("CSP-safe ndarray preserves scalar, nil, negative-stride, and generic stores", () => {
  const safeNil = cspNdarray();
  assert.equal(safeNil.dimension, -1);
  assert.equal(safeNil.size, 0);
  assert.deepEqual(safeNil.shape, []);
  assert.equal(safeNil.pick(), null);

  const dynamicScalar = dynamicNdarray([7], []);
  const safeScalar = cspNdarray([7], []);
  assertEquivalent(safeScalar, dynamicScalar);
  assert.equal(safeScalar.valueOf(), dynamicScalar.valueOf());
  safeScalar.set(9);
  dynamicScalar.set(9);
  assert.equal(safeScalar.get(), dynamicScalar.get());

  const dynamicReverse = dynamicNdarray([1, 2, 3, 4], [4], [-1]);
  const safeReverse = cspNdarray([1, 2, 3, 4], [4], [-1]);
  assertEquivalent(safeReverse, dynamicReverse);
  assert.deepEqual(
    [0, 1, 2, 3].map((index) => safeReverse.get(index)),
    [0, 1, 2, 3].map((index) => dynamicReverse.get(index)),
  );

  const safeStoreValues = new Map([[0, 3], [1, 4]]);
  const dynamicStoreValues = new Map([[0, 3], [1, 4]]);
  const safeStore = {
    get: (index) => safeStoreValues.get(index),
    set: (index, value) => safeStoreValues.set(index, value),
  };
  const dynamicStore = {
    get: (index) => dynamicStoreValues.get(index),
    set: (index, value) => dynamicStoreValues.set(index, value),
  };
  const safeGeneric = cspNdarray(safeStore, [2]);
  const dynamicGeneric = dynamicNdarray(dynamicStore, [2]);
  assertEquivalent(safeGeneric, dynamicGeneric);
  safeGeneric.set(1, 11);
  dynamicGeneric.set(1, 11);
  assert.equal(safeGeneric.get(1), dynamicGeneric.get(1));
});

test("Vite transform removes all dynamic execution from pinned IMG.LY ESM", () => {
  const plugin = cspSafeBackgroundRemovalPlugin();
  const source = readFileSync(imglyEntry, "utf8");
  const result = plugin.transform(source, imglyEntry);

  assert.ok(result?.code);
  assert.match(result.code, /virtual:pet-manager-ndarray-csp/);
  assert.match(result.code, /module\.exports = ndarrayCsp/);
  assert.match(result.code, /freeGlobal_default \|\| freeSelf \|\| globalThis/);
  assert.doesNotMatch(result.code, /(?:^|[^\w$])(?:new\s+)?Function\s*\(/);
  assert.doesNotMatch(result.code, /(?:^|[^\w$])eval\s*\(/);
});

test("Vite transform removes the ONNX Runtime dynamic-global fallbacks", () => {
  const plugin = cspSafeBackgroundRemovalPlugin();
  for (const entry of ortEntries) {
    const source = readFileSync(entry, "utf8");
    const result = plugin.transform(source, entry);
    assert.ok(result?.code);
    assert.match(result.code, /typeof globalThis=="object"\?globalThis:globalThis/);
    assert.doesNotMatch(result.code, /(?:^|[^\w$])(?:new\s+)?Function\s*\(/);
    assert.doesNotMatch(result.code, /(?:^|[^\w$])eval\s*\(/);
  }
});

test("Vite output hook also hardens copied ONNX Runtime worker assets", () => {
  const plugin = cspSafeBackgroundRemovalPlugin();
  const bundle = Object.fromEntries(ortEntries.map((entry, index) => [
    `assets/${index === 0 ? "ort" : "ort.webgpu"}.bundle.min-test.mjs`,
    { type: "asset", source: readFileSync(entry, "utf8") },
  ]));

  plugin.generateBundle({}, bundle);
  for (const output of Object.values(bundle)) {
    assert.doesNotMatch(output.source, /(?:^|[^\w$])(?:new\s+)?Function\s*\(/);
    assert.doesNotMatch(output.source, /(?:^|[^\w$])eval\s*\(/);
  }
});

test("Vite transform fails closed when the pinned IMG.LY dependency layout changes", () => {
  const plugin = cspSafeBackgroundRemovalPlugin();
  assert.throws(
    () => plugin.transform("export default {};", imglyEntry),
    /could not locate the pinned ndarray 1\.0\.19 bundle/,
  );
});
