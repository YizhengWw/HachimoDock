/**
 * [Input] none or a provider config object.
 * [Output] exported Ark-safe 400x300 image defaults plus 5-second Volcano fast-generation resolver consumed by run.js.
 * [Pos] lib node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update run.js, image-processing.js headers.
 */

/** Max longest edge (px) after matting + black composite; 400 keeps 4:3 height at Ark's 300px minimum. */
export const PIPELINE_MAX_IMAGE_DIMENSION = 400;
export const STANDARD_PIPELINE_MAX_IMAGE_DIMENSION = 512;
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
