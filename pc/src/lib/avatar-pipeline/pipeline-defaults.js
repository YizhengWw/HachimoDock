/**
 * [Input] none or a provider config object.
 * [Output] 768/1024px video references, separate 400px vision-analysis input, and 5-second fast video defaults; these are not retried/changed to address image moderation.
 * [Pos] lib node in pc/src/lib/avatar-pipeline
 * [Sync] If this file changes, update run.js, image-processing.js headers.
 */

/** Preserve video-reference detail; vision analysis uses a separate smaller copy. */
export const PIPELINE_MAX_IMAGE_DIMENSION = 768;
export const STANDARD_PIPELINE_MAX_IMAGE_DIMENSION = 1024;
export const THINKING_IMAGE_DIMENSION = 400;
export const PIPELINE_OUTPUT_ASPECT_RATIO = Object.freeze({
  width: 4,
  height: 3,
  label: "4:3",
});

export const FAST_VIDEO_GENERATION_PROFILE = Object.freeze({
  imageMaxDimension: PIPELINE_MAX_IMAGE_DIMENSION,
  volcengine: Object.freeze({
    duration: 5,
    ratio: PIPELINE_OUTPUT_ASPECT_RATIO.label,
    resolution: "480p",
  }),
  kling: Object.freeze({
    duration: 5,
    mode: "standard",
  }),
  custom: Object.freeze({
    duration: 2,
    ratio: PIPELINE_OUTPUT_ASPECT_RATIO.label,
    resolution: "480p",
    quality: "low",
  }),
});

function pickExplicit(value, fallback) {
  return value == null || value === "" ? fallback : value;
}

function readPositiveNumber(value, fallback) {
  const num = Number(value);
  return Number.isFinite(num) && num > 0 ? num : fallback;
}

export function resolveGenerationSpeedConfig(providerConfig = {}) {
  const fastGeneration = providerConfig.fastGeneration !== false;
  const provider = providerConfig.provider || "volcengine";
  const profile = fastGeneration ? FAST_VIDEO_GENERATION_PROFILE[provider] || {} : {};
  const imageMaxDimension = readPositiveNumber(
    providerConfig.imageMaxDimension,
    fastGeneration
      ? FAST_VIDEO_GENERATION_PROFILE.imageMaxDimension
      : STANDARD_PIPELINE_MAX_IMAGE_DIMENSION,
  );

  return {
    fastGeneration,
    imageMaxDimension,
    providerConfig: {
      ...providerConfig,
      duration: pickExplicit(providerConfig.duration, profile.duration),
      ratio: pickExplicit(providerConfig.ratio, profile.ratio || PIPELINE_OUTPUT_ASPECT_RATIO.label),
      resolution: pickExplicit(providerConfig.resolution, profile.resolution),
      quality: pickExplicit(providerConfig.quality, profile.quality),
      mode: pickExplicit(providerConfig.mode, profile.mode),
    },
  };
}
