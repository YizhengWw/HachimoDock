/**
 * [Input] image-processing layout helpers for avatar reference frames.
 * [Output] Node regression coverage for 4:3 black-canvas compositing geometry, alpha matte decontamination,
 *          progress passthrough, and Ark minimum size.
 * [Pos] test node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { computeFourThreeCanvasLayout, decontaminateAlphaMattePixels } from "./image-processing.js";

const srcDir = dirname(fileURLToPath(import.meta.url));

test("four-three canvas layout uses Ark-safe 400x300 in fast mode and fits wide images", () => {
  const layout = computeFourThreeCanvasLayout({
    sourceWidth: 1600,
    sourceHeight: 900,
    maxDimension: 400,
  });

  assert.equal(layout.canvasWidth, 400);
  assert.equal(layout.canvasHeight, 300);
  assert.equal(layout.drawWidth, 400);
  assert.equal(layout.drawHeight, 224);
  assert.equal(layout.drawX, 0);
  assert.equal(layout.drawY, 38);
});

test("four-three canvas layout centers portrait cutouts on black background", () => {
  const layout = computeFourThreeCanvasLayout({
    sourceWidth: 1000,
    sourceHeight: 2000,
    maxDimension: 320,
  });

  assert.equal(layout.canvasWidth, 320);
  assert.equal(layout.canvasHeight, 240);
  assert.equal(layout.drawWidth, 120);
  assert.equal(layout.drawHeight, 240);
  assert.equal(layout.drawX, 100);
  assert.equal(layout.drawY, 0);
});

test("standard mode four-three layout uses a larger 512x384 canvas", () => {
  const layout = computeFourThreeCanvasLayout({
    sourceWidth: 900,
    sourceHeight: 900,
    maxDimension: 512,
  });

  assert.equal(layout.canvasWidth, 512);
  assert.equal(layout.canvasHeight, 384);
  assert.equal(layout.drawWidth, 384);
  assert.equal(layout.drawHeight, 384);
  assert.equal(layout.drawX, 64);
  assert.equal(layout.drawY, 0);
});

test("alpha matte decontamination prevents pale subjects blending into black", () => {
  const imageData = {
    data: new Uint8ClampedArray([
      250, 248, 244, 128,
      255, 255, 255, 8,
      32, 28, 24, 255,
    ]),
  };

  decontaminateAlphaMattePixels(imageData, { foregroundAlphaThreshold: 16 });

  assert.deepEqual(Array.from(imageData.data), [
    250, 248, 244, 255,
    255, 255, 255, 0,
    32, 28, 24, 255,
  ]);
});

test("background-removal progress is forwarded to pipeline callers", () => {
  const source = readFileSync(join(srcDir, "image-processing.js"), "utf8");

  assert.match(source, /progress:\s*\(_key,\s*current,\s*total\) =>/);
  assert.match(source, /onProgress\?\.\("removing_bg",\s*current \/ total\)/);
});
