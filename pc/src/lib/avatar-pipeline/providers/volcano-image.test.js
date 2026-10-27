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
  getArkImageEditPolicyRejection,
} from "./volcano-image.js";

test("image edit moderation distinguishes input, output and generic policy without inventing copyright", () => {
  const error=(code,message="rejected")=>`Ark image edit (fixture) HTTP 400: ${JSON.stringify({error:{code,message}})}`;
  assert.match(getArkImageEditPolicyRejection(error("OutputImageSensitiveContentDetected.PolicyViolation")).message,/处理后的生成结果未通过服务端内容审核/);
  assert.doesNotMatch(getArkImageEditPolicyRejection(error("OutputImageSensitiveContentDetected.PolicyViolation")).message,/版权/);
  assert.match(getArkImageEditPolicyRejection(error("InputImageSensitiveContentDetected.PolicyViolation","copyright restrictions")).message,/输入参考图可能涉及版权限制/);
  assert.match(getArkImageEditPolicyRejection(error("InputTextSensitiveContentDetected.PolicyViolation")).message,/图片处理提示词/);
  assert.equal(getArkImageEditPolicyRejection(error("InvalidParameter","unsupported size")),null);
  assert.equal(getArkImageEditPolicyRejection(error("OutputImageSensitiveContentDetected.PolicyViolation").replace("Ark image edit (fixture)","volcano submit")),null);
  assert.equal(getArkImageEditPolicyRejection(error("OutputImageSensitiveContentDetected.PolicyViolation","Request id: sk-private" )).requestId,null);
});

test("Seedream 5 image edit preserves one input and requests one PNG result", () => {
  const payload = buildVolcanoImageEditPayload({
    imageDataUrl: "data:image/png;base64,iVBORw0KGgo=",
  });

  assert.equal(payload.model, DEFAULT_VOLCANO_IMAGE_MODEL);
  assert.equal(payload.model, "doubao-seedream-5-0-flash-260915");
  assert.deepEqual(payload.image, ["data:image/png;base64,iVBORw0KGgo="]);
  assert.equal(payload.sequential_image_generation, undefined);
  assert.equal(payload.response_format, "b64_json");
  assert.equal(payload.output_format, "png");
  assert.equal(payload.watermark, false);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT, /严格保留输入图片中的主体/);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT, /纯黑色 #000000/);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT, /真人手、手指、手臂、袖口/);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT, /不要误删拟人角色自己的肢体/);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT, /最小必要补全/);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT, /不要裁切耳朵、头部、脚、尾巴、球拍等相关物件或一体化底座/);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT, /不输出头像或半身特写/);
  assert.match(BLACK_BACKGROUND_EDIT_PROMPT, /约10%的安全空间/);
});

test("older Seedream fallback omits the 5.0-only output format", () => {
  const payload = buildVolcanoImageEditPayload({
    model: "doubao-seedream-4-5-251128",
    imageDataUrl: "data:image/jpeg;base64,/9j/",
  });

  assert.equal(payload.output_format, undefined);
  assert.equal(payload.sequential_image_generation, "disabled");
});

test("legacy image alias resolves only to the same-version canonical ID", () => {
  const input={imageDataUrl:"data:image/png;base64,abc"};
  assert.equal(buildVolcanoImageEditPayload({...input,model:"doubao-seedream-5-0-lite-260128"}).model,"doubao-seedream-5-0-260128");
  assert.equal(buildVolcanoImageEditPayload({...input,model:"ep-private"}).model,"ep-private");
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
