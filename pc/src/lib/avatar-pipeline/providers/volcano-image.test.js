/**
 * [Input] Ark Seedream image-edit model configuration and representative API envelopes.
 * [Output] Node regression coverage for black-background payload shape, model compatibility, and response extraction.
 * [Pos] test node in pc/src/lib/avatar-pipeline/providers
 * [Sync] If this file changes, update pc/src/.folder.md.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  BLACK_BACKGROUND_EDIT_PROMPT,
  DEFAULT_VOLCANO_IMAGE_MODEL,
  buildVolcanoImageEditPayload,
  extractVolcanoImageResult,
} from "./volcano-image.js";

test("Seedream 5 image edit preserves one input and requests one PNG result", () => {
  const payload = buildVolcanoImageEditPayload({
    imageDataUrl: "data:image/png;base64,iVBORw0KGgo=",
  });

  assert.equal(payload.model, DEFAULT_VOLCANO_IMAGE_MODEL);
  assert.deepEqual(payload.image, ["data:image/png;base64,iVBORw0KGgo="]);
  assert.equal(payload.sequential_image_generation, "disabled");
  assert.equal(payload.response_format, "b64_json");
  assert.equal(payload.output_format, "png");
  assert.equal(payload.watermark, false);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT, /严格保留输入图片中的主体/);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT, /纯黑色 #000000/);
});

test("older Seedream fallback omits the 5.0-only output format", () => {
  const payload = buildVolcanoImageEditPayload({
    model: "doubao-seedream-4-5-251128",
    imageDataUrl: "data:image/jpeg;base64,/9j/",
  });

  assert.equal(payload.output_format, undefined);
});

test("Ark image result accepts base64 and URL response forms", () => {
  assert.deepEqual(extractVolcanoImageResult({ data: [{ b64_json: "abc" }] }), {
    base64: "abc",
    url: "",
  });
  assert.deepEqual(extractVolcanoImageResult({ data: [{ url: "https://example.test/a.png" }] }), {
    base64: "",
    url: "https://example.test/a.png",
  });
});
