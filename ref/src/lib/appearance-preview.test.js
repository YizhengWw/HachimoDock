/**
 * [Input] Appearance manifest preview helper fixtures.
 * [Output] Node test coverage for gallery and family thumbnail media selection.
 * [Pos] test node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";

import {
  pickPreviewFamily,
  resolveGeneratedPreviewMedia,
  resolveGalleryPreviewMedia,
} from "./appearance-preview.js";

test("pickPreviewFamily prefers idle.default over earlier generated videos", () => {
  const family = pickPreviewFamily([
    { family: "touch.right", ok: true, videoPath: "videos/touch.right.mp4" },
    { family: "idle.default", ok: true, videoPath: "videos/idle.default.mp4" },
  ]);

  assert.equal(family.family, "idle.default");
});

test("pickPreviewFamily prefers custom working before built-in working variants and legacy working.default", () => {
  const family = pickPreviewFamily([
    { family: "working.default", ok: true, videoPath: "videos/working.default.mp4" },
    { family: "working.thinking", ok: true, videoPath: "videos/working.thinking.mp4" },
    { family: "working", ok: true, videoPath: "videos/working.mp4" },
  ]);

  assert.equal(family.family, "working");
});

test("resolveGalleryPreviewMedia falls back to source image when no video is usable", () => {
  const preview = resolveGalleryPreviewMedia({
    name: "毛扎扎",
    source_image: "custom-appearances/pet/source.png",
    source_mime: "image/png",
    families: [
      { family: "welcome", ok: false, error: "pending" },
      { family: "idle.default", ok: true },
    ],
  });

  assert.deepEqual(preview, {
    kind: "image",
    src: "",
    path: "custom-appearances/pet/source.png",
    mime: "image/png",
    label: "毛扎扎",
  });
});

test("resolveGalleryPreviewMedia prefers generated video for animated gallery cards", () => {
  const preview = resolveGalleryPreviewMedia({
    type: "codex-import",
    name: "RX-93",
    source_preview: "custom-appearances/codex-rx93/preview.png",
    source_preview_src: "asset://custom-appearances/codex-rx93/preview.png",
    source_preview_mime: "image/png",
    source_image: "custom-appearances/codex-rx93/source.webp",
    source_mime: "image/webp",
    families: [
      {
        family: "idle.default",
        ok: true,
        videoPath: "custom-appearances/codex-rx93/videos/idle.default.mp4",
        videoSrc: "asset://custom-appearances/codex-rx93/videos/idle.default.mp4",
      },
    ],
  });

  assert.deepEqual(preview, {
    kind: "video",
    src: "asset://custom-appearances/codex-rx93/videos/idle.default.mp4",
    path: "custom-appearances/codex-rx93/videos/idle.default.mp4",
    mime: "video/mp4",
    label: "idle.default",
    fallback: {
      kind: "image",
      src: "asset://custom-appearances/codex-rx93/preview.png",
      path: "custom-appearances/codex-rx93/preview.png",
      mime: "image/png",
      label: "RX-93",
    },
  });
});

test("resolveGeneratedPreviewMedia keeps generated videos for family previews", () => {
  const preview = resolveGeneratedPreviewMedia({
    type: "codex-import",
    name: "RX-93",
    source_preview: "custom-appearances/codex-rx93/preview.png",
    source_preview_src: "asset://custom-appearances/codex-rx93/preview.png",
    source_preview_mime: "image/png",
    source_image: "custom-appearances/codex-rx93/source.webp",
    source_mime: "image/webp",
    families: [
      {
        family: "idle.default",
        ok: true,
        videoPath: "custom-appearances/codex-rx93/videos/idle.default.mp4",
        videoSrc: "asset://custom-appearances/codex-rx93/videos/idle.default.mp4",
      },
    ],
  });

  assert.deepEqual(preview, {
    kind: "video",
    src: "asset://custom-appearances/codex-rx93/videos/idle.default.mp4",
    path: "custom-appearances/codex-rx93/videos/idle.default.mp4",
    mime: "video/mp4",
    label: "idle.default",
    fallback: {
      kind: "image",
      src: "asset://custom-appearances/codex-rx93/preview.png",
      path: "custom-appearances/codex-rx93/preview.png",
      mime: "image/png",
      label: "RX-93",
    },
  });
});
