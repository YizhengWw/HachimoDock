/**
 * [Input] image-processing layout helpers for avatar reference frames.
 * [Output] Node regression coverage for 4:3 black-canvas compositing geometry and Ark minimum size.
 * [Pos] test node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { computeFourThreeCanvasLayout } from "./image-processing.js";

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
