/**
 * [Input] user-uploaded image Blob.
 * [Output] background-removed, black-composited, fixed 4:3 downscaled PNG as Uint8Array.
 * [Pos] lib node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header, run.js, and pipeline-defaults.js.
 */

import { removeBackground } from "@imgly/background-removal";
import {
  PIPELINE_MAX_IMAGE_DIMENSION,
  PIPELINE_OUTPUT_ASPECT_RATIO,
} from "./pipeline-defaults.js";

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
 * Remove background, composite onto a black 4:3 canvas, and downscale.
 *
 * @param {Blob} imageBlob
 * @param {object} [options]
 * @param {number} [options.maxDimension]
 * @param {(stage: 'removing_bg'|'compositing'|'done', progress?: number) => void} [options.onProgress]
 * @param {AbortSignal} [options.signal]
 * @returns {Promise<{ processedBytes: Uint8Array, width: number, height: number }>}
 */
export async function processImageForPipeline(imageBlob, options = {}) {
  const { maxDimension = PIPELINE_MAX_IMAGE_DIMENSION, onProgress, signal } = options;

  if (signal?.aborted) throw new DOMException("aborted", "AbortError");

  // Stage 1: Background removal
  onProgress?.("removing_bg");
  const transparentBlob = await removeBackground(imageBlob, {
    progress: (key, current, total) => {
      // @imgly/background-removal fires progress events during model download
    },
  });

  if (signal?.aborted) throw new DOMException("aborted", "AbortError");

  // Stage 2: Composite onto black 4:3 background + downscale
  onProgress?.("compositing");
  const img = await loadImage(transparentBlob);
  const { naturalWidth, naturalHeight } = img;
  const layout = computeFourThreeCanvasLayout({
    sourceWidth: naturalWidth,
    sourceHeight: naturalHeight,
    maxDimension,
  });

  const canvas = document.createElement("canvas");
  canvas.width = layout.canvasWidth;
  canvas.height = layout.canvasHeight;
  const ctx = canvas.getContext("2d");
  ctx.fillStyle = "#000000";
  ctx.fillRect(0, 0, layout.canvasWidth, layout.canvasHeight);
  ctx.drawImage(img, layout.drawX, layout.drawY, layout.drawWidth, layout.drawHeight);

  const processedBytes = await canvasToUint8Array(canvas);

  if (signal?.aborted) throw new DOMException("aborted", "AbortError");

  onProgress?.("done");
  return { processedBytes, width: layout.canvasWidth, height: layout.canvasHeight };
}
