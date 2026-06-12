/**
 * [Input] user-uploaded image File / Blob.
 * [Output] base64 dataURL + mime utilities consumed by thinking-model + per-provider task submission.
 * [Pos] lib node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header and provider modules that depend on these helpers.
 */

const MIME_BY_EXT = {
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".jpeg": "image/jpeg",
  ".webp": "image/webp",
  ".gif": "image/gif",
};

export function mimeFromExtension(filename) {
  const ext = filename.toLowerCase().slice(filename.lastIndexOf("."));
  return MIME_BY_EXT[ext] || "";
}

export function extensionFromMime(mime) {
  switch ((mime || "").toLowerCase()) {
    case "image/png":
      return ".png";
    case "image/jpeg":
      return ".jpg";
    case "image/webp":
      return ".webp";
    case "image/gif":
      return ".gif";
    default:
      return ".png";
  }
}

/** Convert a Uint8Array to base64 in chunks (avoids the 2^17 String.fromCharCode arg limit). */
export function uint8ToBase64(bytes) {
  const CHUNK = 0x8000;
  let binary = "";
  for (let i = 0; i < bytes.length; i += CHUNK) {
    binary += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
  }
  return btoa(binary);
}

export async function fileToBytes(file) {
  const buf = await file.arrayBuffer();
  return new Uint8Array(buf);
}

export function bytesToDataUrl(bytes, mime) {
  return `data:${mime};base64,${uint8ToBase64(bytes)}`;
}

/**
 * Build a single image payload object the pipeline can hand to thinking + each provider,
 * doing the heavy base64 encode only once.
 *
 * @param {File} file
 * @returns {Promise<{ bytes: Uint8Array, mime: string, base64: string, dataUrl: string, filename: string }>}
 */
export async function buildImagePayload(file) {
  const bytes = await fileToBytes(file);
  const mime = file.type || mimeFromExtension(file.name) || "image/png";
  const base64 = uint8ToBase64(bytes);
  return {
    bytes,
    mime,
    base64,
    dataUrl: `data:${mime};base64,${base64}`,
    filename: file.name,
  };
}
