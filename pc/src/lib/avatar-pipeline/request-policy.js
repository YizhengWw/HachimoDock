/** Finite image-body budget and caller cancellation. No request is retried here. */
export const IMAGE_EDIT_TOTAL_TIMEOUT_MS = 600_000;
export const DEFAULT_HTTP_TOTAL_TIMEOUT_MS = 120_000;

export function generationRequestTimeoutMs(init = {}) {
  const image = init.diagnosticStage === "image_edit"
    && String(init.method || "GET").toUpperCase() === "POST";
  const supplied = Number.isFinite(init.timeoutMs) ? Math.max(1, Math.floor(init.timeoutMs)) : undefined;
  return image ? Math.min(supplied ?? DEFAULT_HTTP_TOTAL_TIMEOUT_MS, IMAGE_EDIT_TOTAL_TIMEOUT_MS)
    : supplied;
}

export function waitForNativeGeneration(request, signal) {
  if (signal?.aborted) return Promise.reject(new DOMException("aborted", "AbortError"));
  if (!signal) return Promise.resolve().then(request);
  return new Promise((resolve, reject) => {
    let settled = false;
    const finish = (fn, value) => {
      if (settled) return;
      settled = true;
      signal.removeEventListener("abort", onAbort);
      fn(value);
    };
    // Cancellation ends local use only. Native/server work may already be in
    // flight and billable; leave the durable imageIntent intact, never resubmit.
    const onAbort = () => finish(reject, new DOMException("aborted", "AbortError"));
    signal.addEventListener("abort", onAbort, { once: true });
    Promise.resolve().then(() => {
      if (signal.aborted) throw new DOMException("aborted", "AbortError");
      return request();
    }).then(value => finish(resolve, value), error => finish(reject, error));
  });
}
