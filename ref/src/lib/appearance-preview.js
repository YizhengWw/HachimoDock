/**
 * [Input] Appearance records produced by `appearance-store.js`.
 * [Output] Pure media-selection helpers for gallery cards and family thumbnails with full static source previews
 *          for appearance cards, generated-video descriptors for family previews, and fallback descriptors.
 * [Pos] lib node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

const PREFERRED_PREVIEW_FAMILIES = [
  "idle.default",
  "welcome",
  "idle.playing",
  "idle.wandering",
  "working",
  "working.thinking",
  "working.default",
  "done",
];

function hasUsableVideo(family) {
  return Boolean(family?.ok && (family.videoSrc || family.videoPath));
}

export function pickPreviewFamily(families = []) {
  const usable = (families || []).filter(hasUsableVideo);
  if (usable.length === 0) return null;
  for (const familyName of PREFERRED_PREVIEW_FAMILIES) {
    const preferred = usable.find((family) => family.family === familyName);
    if (preferred) return preferred;
  }
  return usable[0];
}

export function mediaFromFamily(family, record) {
  if (!hasUsableVideo(family)) return null;
  return {
    kind: "video",
    src: family.videoSrc || "",
    path: family.videoPath || "",
    mime: "video/mp4",
    label: family.family || "Video preview",
    fallback: mediaFromSourcePreview(record) || mediaFromSourceImage(record),
  };
}

export function mediaFromSourceImage(record) {
  if (!record?.source_image_src && !record?.source_image) return null;
  return {
    kind: "image",
    src: record.source_image_src || "",
    path: record.source_image || "",
    mime: record.source_mime || "image/png",
    label: record.name || "Source image preview",
  };
}

export function mediaFromSourcePreview(record) {
  if (!record?.source_preview_src && !record?.source_preview) return null;
  return {
    kind: "image",
    src: record.source_preview_src || "",
    path: record.source_preview || "",
    mime: record.source_preview_mime || "image/png",
    label: record.name || "Source preview",
  };
}

export function resolveGalleryPreviewMedia(record) {
  return mediaFromFamily(pickPreviewFamily(record?.families), record) ||
    mediaFromSourcePreview(record) ||
    mediaFromSourceImage(record) ||
    {
      kind: "empty",
      src: "",
      path: "",
      mime: "",
      label: record?.name || "Preview",
    };
}

export function resolveGeneratedPreviewMedia(record) {
  return mediaFromFamily(pickPreviewFamily(record?.families), record) ||
    mediaFromSourcePreview(record) ||
    mediaFromSourceImage(record) || {
      kind: "empty",
      src: "",
      path: "",
      mime: "",
      label: record?.name || "Preview",
    };
}

export function resolveDashboardPreviewMedia(record) {
  return resolveGalleryPreviewMedia(record);
}
