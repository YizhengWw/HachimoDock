/**
 * [Input] user-uploaded image Blob plus optional Ark cloud-background configuration.
 * [Output] Ark-edited (or original fallback), alpha-normalized, black-composited, fixed 4:3 downscaled PNG as Uint8Array.
 * [Pos] lib node in pc/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header, run.js, pipeline-defaults.js, providers/volcano-image.js, and pc/src/.folder.md.
 */

import { bytesToDataUrl } from "./image.js";
import {
  PIPELINE_MAX_IMAGE_DIMENSION,
  PIPELINE_OUTPUT_ASPECT_RATIO,
} from "./pipeline-defaults.js";
import { editImageBackgroundWithArk } from "./providers/volcano-image.js";

function loadImage(blob) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(blob);
    const img = new Image();
    img.onload = () => {
      URL.revokeObjectURL(url);
      resolve(img);
    };
    img.onerror = (e) => {
      URL.revokeObjectURL(url);
      reject(new Error("Failed to decode image for processing"));
    };
    img.src = url;
  });
}

function canvasToUint8Array(canvas) {
  return new Promise((resolve, reject) => {
    canvas.toBlob(
      (blob) => {
        if (!blob) {
          reject(new Error("Canvas toBlob returned null"));
          return;
        }
        blob.arrayBuffer().then((buf) => resolve(new Uint8Array(buf)));
      },
      "image/png",
    );
  });
}

function even(value) {
  const rounded = Math.max(2, Math.round(value));
  return rounded % 2 === 0 ? rounded : rounded - 1;
}

export function decontaminateAlphaMattePixels(imageData, { foregroundAlphaThreshold = 16 } = {}) {
  const { data } = imageData;
  for (let index = 0; index < data.length; index += 4) {
    const alpha = data[index + 3];
    data[index + 3] = alpha >= foregroundAlphaThreshold ? 255 : 0;
  }
  return imageData;
}

export function computeFourThreeCanvasLayout({
  sourceWidth,
  sourceHeight,
  maxDimension = PIPELINE_MAX_IMAGE_DIMENSION,
  aspectRatio = PIPELINE_OUTPUT_ASPECT_RATIO,
} = {}) {
  const width = Number(sourceWidth);
  const height = Number(sourceHeight);
  if (!Number.isFinite(width) || width <= 0 || !Number.isFinite(height) || height <= 0) {
    throw new Error("sourceWidth and sourceHeight must be positive numbers");
  }

  const ratioW = aspectRatio.width || 4;
  const ratioH = aspectRatio.height || 3;
  const canvasWidth = even(maxDimension);
  const canvasHeight = even((canvasWidth * ratioH) / ratioW);
  const scale = Math.min(canvasWidth / width, canvasHeight / height);
  const drawWidth = even(width * scale);
  const drawHeight = even(height * scale);
  const drawX = Math.round((canvasWidth - drawWidth) / 2);
  const drawY = Math.round((canvasHeight - drawHeight) / 2);

  return { canvasWidth, canvasHeight, drawX, drawY, drawWidth, drawHeight };
}

/**
 * Ask Ark to replace the background, then composite onto a black 4:3 canvas and downscale.
 * If Ark is unavailable, preserve the source image and still produce a valid reference frame.
 *
 * @param {Blob} imageBlob
 * @param {object} [options]
 * @param {number} [options.maxDimension]
 * @param {{ apiKey: string, baseUrl?: string, model?: string } | null} [options.cloudBackground]
 * @param {(stage: 'cloud_editing'|'cloud_fallback'|'compositing'|'done', progress?: number, detail?: string) => void} [options.onProgress]
 * @param {AbortSignal} [options.signal]
 * @returns {Promise<{ processedBytes: Uint8Array, width: number, height: number }>}
 */
export async function processImageForPipeline(imageBlob, options = {}) {
  const {
    maxDimension = PIPELINE_MAX_IMAGE_DIMENSION,
    cloudBackground = null,
    onProgress,
    signal,
  } = options;

  if (signal?.aborted) throw new DOMException("aborted", "AbortError");

  let sourceBlob = imageBlob;
  let backgroundMode = "original";
  let backgroundModel = "";
  let backgroundWarning = "";
  if (cloudBackground?.apiKey) {
    onProgress?.("cloud_editing");
    try {
      const inputBytes = new Uint8Array(await imageBlob.arrayBuffer());
      const inputMime = imageBlob.type || "image/png";
      const edited = await editImageBackgroundWithArk({
        ...cloudBackground,
        imageDataUrl: bytesToDataUrl(inputBytes, inputMime),
        signal,
      });
      sourceBlob = new Blob([edited.bytes], { type: edited.mime });
      backgroundMode = "ark";
      backgroundModel = edited.model;
    } catch (error) {
      if (signal?.aborted) throw new DOMException("aborted", "AbortError");
      backgroundWarning = error?.message || String(error);
      console.warn("[avatar-pipeline] Ark background edit failed; using original image:", error);
      onProgress?.("cloud_fallback", undefined, backgroundWarning);
    }
  } else {
    backgroundWarning = "Ark cloud background editing is unavailable for the selected provider";
    onProgress?.("cloud_fallback", undefined, backgroundWarning);
  }

  if (signal?.aborted) throw new DOMException("aborted", "AbortError");

  // Stage 2: Composite onto black 4:3 background + downscale
  onProgress?.("compositing");
  const img = await loadImage(sourceBlob);
  const { naturalWidth, naturalHeight } = img;
  const layout = computeFourThreeCanvasLayout({
    sourceWidth: naturalWidth,
    sourceHeight: naturalHeight,
    maxDimension,
  });

  const matteCanvas = document.createElement("canvas");
  matteCanvas.width = layout.canvasWidth;
  matteCanvas.height = layout.canvasHeight;
  const matteCtx = matteCanvas.getContext("2d");
  matteCtx.clearRect(0, 0, layout.canvasWidth, layout.canvasHeight);
  matteCtx.drawImage(img, layout.drawX, layout.drawY, layout.drawWidth, layout.drawHeight);
  const matteData = matteCtx.getImageData(0, 0, layout.canvasWidth, layout.canvasHeight);
  decontaminateAlphaMattePixels(matteData);
  matteCtx.putImageData(matteData, 0, 0);

  const canvas = document.createElement("canvas");
  canvas.width = layout.canvasWidth;
  canvas.height = layout.canvasHeight;
  const ctx = canvas.getContext("2d");
  ctx.fillStyle = "#000000";
  ctx.fillRect(0, 0, layout.canvasWidth, layout.canvasHeight);
  ctx.drawImage(matteCanvas, 0, 0);

  const processedBytes = await canvasToUint8Array(canvas);

  if (signal?.aborted) throw new DOMException("aborted", "AbortError");

  onProgress?.("done");
  return {
    processedBytes,
    width: layout.canvasWidth,
    height: layout.canvasHeight,
    backgroundMode,
    backgroundModel,
    backgroundWarning,
  };
}
