/**
 * [Input] avatar pipeline generation-speed defaults.
 * [Output] Node regression coverage for fast low-resolution 5-second provider config resolution.
 * [Pos] test node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  FAST_VIDEO_GENERATION_PROFILE,
  PIPELINE_OUTPUT_ASPECT_RATIO,
  PIPELINE_MAX_IMAGE_DIMENSION,
  resolveGenerationSpeedConfig,
} from "./pipeline-defaults.js";

test("fast generation profile keeps Volcano reference frames above Ark minimum height", () => {
  assert.equal(PIPELINE_MAX_IMAGE_DIMENSION, 400);
  assert.equal(FAST_VIDEO_GENERATION_PROFILE.imageMaxDimension, 400);

  const resolved = resolveGenerationSpeedConfig({
    provider: "volcengine",
    model: "doubao-seedance-1-5-pro-251215",
  });

  assert.equal(resolved.fastGeneration, true);
  assert.equal(resolved.imageMaxDimension, 400);
  assert.equal(resolved.providerConfig.duration, 5);
  assert.equal(resolved.providerConfig.ratio, PIPELINE_OUTPUT_ASPECT_RATIO.label);
  assert.equal(resolved.providerConfig.resolution, "480p");
});

test("fast generation profile keeps Kling on standard short clips", () => {
  const resolved = resolveGenerationSpeedConfig({
    provider: "kling",
    model: "kling-v1-5",
  });

  assert.equal(resolved.providerConfig.mode, "standard");
  assert.equal(resolved.providerConfig.duration, 5);
});

test("explicit generation settings override the fast profile", () => {
  const resolved = resolveGenerationSpeedConfig({
    provider: "volcengine",
    fastGeneration: true,
    imageMaxDimension: 512,
    duration: 4,
    resolution: "720p",
  });

  assert.equal(resolved.imageMaxDimension, 512);
  assert.equal(resolved.providerConfig.duration, 4);
  assert.equal(resolved.providerConfig.ratio, PIPELINE_OUTPUT_ASPECT_RATIO.label);
  assert.equal(resolved.providerConfig.resolution, "720p");
});

test("standard generation disables fast provider settings and uses a larger reference image", () => {
  const resolved = resolveGenerationSpeedConfig({
    provider: "volcengine",
    fastGeneration: false,
  });

  assert.equal(resolved.fastGeneration, false);
  assert.ok(resolved.imageMaxDimension > FAST_VIDEO_GENERATION_PROFILE.imageMaxDimension);
  assert.equal(resolved.providerConfig.ratio, PIPELINE_OUTPUT_ASPECT_RATIO.label);
  assert.equal(resolved.providerConfig.duration, undefined);
  assert.equal(resolved.providerConfig.resolution, undefined);
});
