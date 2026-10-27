// Local reference import: source paths and metadata never leave the native importer.
export const MAX_REFERENCE_BYTES = 32 * 1024 * 1024;
export function referenceFileFromEvent(event, token, FileClass = File) {
  if (event?.token !== token || event.status !== "ready") return null;
  if (!["image/jpeg", "image/png"].includes(event.mime) || typeof event.base64 !== "string"
    || !event.base64.length || event.base64.length > 23 * 1024 * 1024) throw new Error("图片导入结果无效，请重试。");
  const bytes = Uint8Array.from(atob(event.base64), c => c.charCodeAt(0));
  if (bytes.length > 16 * 1024 * 1024) throw new Error("转换后的图片过大。");
  return new FileClass([bytes], event.mime === "image/png" ? "reference.png" : "reference.jpg", { type: event.mime });
}
export function validateReferenceFile(file) {
  if (!file || file.size <= 0) throw new Error("图片为空；若来自 iCloud，请先下载原图。");
  if (file.size > MAX_REFERENCE_BYTES) throw new Error("图片超过 32 MB，请先缩小后重试。");
  const knownType = /^image\/(png|jpe?g|webp|gif|heic|heif|tiff|bmp)$/i.test(file.type || "");
  const emptyType = !file.type || file.type === "application/octet-stream";
  if (!(knownType || (emptyType && /\.(png|jpe?g|webp|gif|heic|heif|tiff?|bmp)$/i.test(file.name || "")))) throw new Error("请选择 PNG、JPEG、WebP、GIF 或 HEIC 等静态图片，不支持 SVG。 ");
}
// Serialize mount/action/dispose so a late mount cannot leave an invisible native receiver behind.
export function createReferenceImportSession({ invoke, listen, token, onEvent }) {
  let disposed = false, unlisten, mounted = false;
  const ready = listen("reference-image", ({ payload }) => {
    if (!disposed && payload?.token === token) onEvent(payload);
  }).then(stop => { if (disposed) stop(); else unlisten = stop; });
  let chain = ready;
  const schedule = task => { const next = chain.then(task); chain = next.catch(() => {}); return next; };
  return {
    mount(rect) { return schedule(async () => {
      if (disposed) return false;
      mounted = await invoke("reference_image_mount", { token, rect }); return mounted;
    }); },
    action(action) { return schedule(() => !disposed && mounted ? invoke("reference_image_action", { token, action }) : undefined); },
    dispose() { disposed = true; unlisten?.(); return schedule(() => mounted ? invoke("reference_image_action", { token, action: "unmount" }) : undefined); },
  };
}
export function visibleReferenceRect(element, viewportWidth = window.innerWidth, viewportHeight = window.innerHeight) {
  const rect = element.getBoundingClientRect();
  let left = Math.max(0, rect.left), top = Math.max(0, rect.top);
  let right = Math.min(viewportWidth, rect.right), bottom = Math.min(viewportHeight, rect.bottom);
  for (let parent = element.parentElement; parent; parent = parent.parentElement) {
    const style = getComputedStyle(parent), clip = parent.getBoundingClientRect();
    if (/(auto|scroll|hidden|clip)/.test(style.overflowX)) { left = Math.max(left, clip.left); right = Math.min(right, clip.right); }
    if (/(auto|scroll|hidden|clip)/.test(style.overflowY)) { top = Math.max(top, clip.top); bottom = Math.min(bottom, clip.bottom); }
  }
  return [left, top, Math.max(0, right - left), Math.max(0, bottom - top), viewportWidth];
}
