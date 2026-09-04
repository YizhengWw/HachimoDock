/**
 * [Input] source image data URL + Volcengine Ark API-key/base-url/model config.
 * [Output] Seedream-edited image bytes with the original subject preserved on a pure black background.
 * [Pos] image-edit provider node in pc/src/lib/avatar-pipeline/providers
 * [Sync] If this file changes, update image-processing.js, run.js, provider-config.js, and pc/src/.folder.md.
 */

import { downloadBinary, pipelineFetch, readJsonOrThrow, withRetry } from "../http.js";

export const VOLCANO_IMAGE_GENERATION_PATH = "/api/v3/images/generations";
export const DEFAULT_VOLCANO_IMAGE_MODEL = "doubao-seedream-5-0-lite-260128";
export const VOLCANO_IMAGE_MODEL_FALLBACKS = Object.freeze([
  "doubao-seedream-4-5-251128",
  "doubao-seedream-4-0-250828",
]);

export const BLACK_BACKGROUND_EDIT_PROMPT = [
  "仅执行背景替换。",
  "严格保留输入图片中的主体身份、外形、毛色或服装、纹理、姿势、表情、比例、朝向、构图和镜头，不得重绘或修改主体。",
  "删除主体以外的全部原背景、文字、水印、边框、阴影和杂物，并将整个背景替换为均匀、无纹理、无渐变、无反光的纯黑色 #000000。",
  "只保留一个原主体，不得新增角色、肢体、道具、文字或装饰。",
].join("");

function joinUrl(baseUrl, path) {
  const trimmed = String(baseUrl || "https://ark.cn-beijing.volces.com").replace(/\/+$/, "");
  return path.startsWith("/") ? `${trimmed}${path}` : `${trimmed}/${path}`;
}

function isSeedreamFive(model) {
  return /seedream-5-0/i.test(String(model || ""));
}

export function buildVolcanoImageEditPayload({
  model = DEFAULT_VOLCANO_IMAGE_MODEL,
  imageDataUrl,
  prompt = BLACK_BACKGROUND_EDIT_PROMPT,
}) {
  if (!imageDataUrl) throw new Error("Ark image editing requires an input image");
  const payload = {
    model,
    prompt,
    image: [imageDataUrl],
    size: "2K",
    sequential_image_generation: "disabled",
    response_format: "b64_json",
    watermark: false,
  };
  if (isSeedreamFive(model)) payload.output_format = "png";
  return payload;
}

function shouldRetryArkImageHttp(error) {
  const message = String(error?.message || error || "");
  const match = /HTTP\s+(\d{3})/.exec(message);
  if (!match) return true;
  const status = Number(match[1]);
  return status === 408 || status === 409 || status === 425 || status === 429 || status >= 500;
}

function isUnavailableModelError(error) {
  return /ModelNotOpen|InvalidEndpointOrModel\.NotFound|has not activated the model|model.+not found/i.test(
    String(error?.message || error || ""),
  );
}

function base64ToBytes(value) {
  const normalized = String(value || "").replace(/^data:[^,]*,/, "");
  const binary = atob(normalized);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) bytes[index] = binary.charCodeAt(index);
  return bytes;
}

function inferMime(bytes, fallback = "image/png") {
  if (bytes.length >= 8 && bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47) {
    return "image/png";
  }
  if (bytes.length >= 3 && bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff) {
    return "image/jpeg";
  }
  if (bytes.length >= 12 && String.fromCharCode(...bytes.slice(8, 12)) === "WEBP") {
    return "image/webp";
  }
  return fallback;
}

export function extractVolcanoImageResult(json) {
  const entry = Array.isArray(json?.data) ? json.data[0] : json?.data?.[0] || json?.data;
  const base64 = entry?.b64_json || entry?.base64 || entry?.image_base64 || "";
  const url = entry?.url || entry?.image_url || "";
  return { base64, url };
}

async function requestImageEdit({ apiKey, baseUrl, model, imageDataUrl, signal }) {
  const apiUrl = joinUrl(baseUrl, VOLCANO_IMAGE_GENERATION_PATH);
  const payload = buildVolcanoImageEditPayload({ model, imageDataUrl });
  const json = await withRetry(
    async () => {
      const response = await pipelineFetch(apiUrl, {
        method: "POST",
        signal,
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${apiKey}`,
        },
        body: JSON.stringify(payload),
      });
      return readJsonOrThrow(response, `Ark image edit (${model})`);
    },
    { retries: 2, signal, shouldRetry: shouldRetryArkImageHttp },
  );

  const result = extractVolcanoImageResult(json);
  let bytes;
  if (result.base64) bytes = base64ToBytes(result.base64);
  else if (result.url) bytes = await downloadBinary(result.url, signal);
  else throw new Error(`Ark image edit (${model}) returned no image data`);

  return {
    bytes,
    mime: inferMime(bytes),
    model: json?.model || model,
    raw: json,
  };
}

/**
 * Edit one image through Ark, falling back only when an account has not opened
 * the preferred Seedream model. Transport, quota, content-safety, and other
 * request failures remain visible to the caller instead of multiplying paid calls.
 */
export async function editImageBackgroundWithArk({
  apiKey,
  baseUrl,
  model = DEFAULT_VOLCANO_IMAGE_MODEL,
  imageDataUrl,
  signal,
}) {
  if (!apiKey) throw new Error("Ark API key is required for cloud background editing");
  const candidates = [model, ...VOLCANO_IMAGE_MODEL_FALLBACKS].filter(
    (candidate, index, list) => candidate && list.indexOf(candidate) === index,
  );
  let unavailableError;
  for (const candidate of candidates) {
    try {
      return await requestImageEdit({
        apiKey,
        baseUrl,
        model: candidate,
        imageDataUrl,
        signal,
      });
    } catch (error) {
      if (!isUnavailableModelError(error)) throw error;
      unavailableError = error;
    }
  }
  throw unavailableError || new Error("No Ark Seedream image model is available");
}
