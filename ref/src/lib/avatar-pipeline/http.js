/**
 * [Input] HTTP request descriptors from provider scripts.
 * [Output] desktop-first HTTP bridge (Rust invoke first, Tauri plugin fallback, browser fallback) + retry helpers.
 * [Pos] lib node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header and the Tauri runtime command list that depends on these helpers.
 */

let cachedTauriFetch; // undefined = not probed yet; null = probed, unavailable
let cachedTauriInvoke; // undefined = not probed; null = unavailable

function isTauriRuntime() {
  return (
    typeof window !== "undefined" &&
    (Boolean(window.__TAURI_INTERNALS__) || Boolean(window.__TAURI__))
  );
}

function normalizeHeaders(headers) {
  if (!headers) return {};
  if (headers instanceof Headers) {
    return Object.fromEntries(headers.entries());
  }
  if (Array.isArray(headers)) {
    return Object.fromEntries(headers);
  }
  return { ...headers };
}

function normalizeBody(body) {
  if (body == null) return null;
  if (typeof body === "string") return body;
  if (body instanceof URLSearchParams) return body.toString();
  return String(body);
}

function isMissingTauriCommand(err) {
  const raw = String(err?.message || err || "");
  return /command .* not found|unknown command|not found/i.test(raw);
}

function buildTransportError(err, { url, method, label }) {
  const raw = err?.message || String(err);
  if (/Failed to fetch|NetworkError|TypeError/i.test(raw)) {
    return new Error(
      `无法连接到 ${url}（${method} via ${label}）。请检查 Base URL、网络、证书，或确认当前运行的是桌面版 Pet Manager。原始错误: ${raw}`,
    );
  }
  return new Error(`${method} ${url} via ${label} failed: ${raw}`);
}

async function getTauriInvoke() {
  if (cachedTauriInvoke !== undefined) return cachedTauriInvoke;
  if (!isTauriRuntime()) {
    cachedTauriInvoke = null;
    return null;
  }
  try {
    const mod = await import("@tauri-apps/api/core");
    cachedTauriInvoke = typeof mod.invoke === "function" ? mod.invoke : null;
  } catch (err) {
    console.warn("[pipelineFetch] failed to load @tauri-apps/api/core:", err);
    cachedTauriInvoke = null;
  }
  return cachedTauriInvoke;
}

async function getTauriFetch() {
  if (cachedTauriFetch !== undefined) return cachedTauriFetch;
  if (!isTauriRuntime()) {
    cachedTauriFetch = null;
    return null;
  }
  try {
    const mod = await import("@tauri-apps/plugin-http");
    if (typeof mod.fetch !== "function") {
      console.warn("[pipelineFetch] plugin-http loaded but `fetch` missing", mod);
      cachedTauriFetch = null;
      return null;
    }
    cachedTauriFetch = mod.fetch;
  } catch (err) {
    console.warn("[pipelineFetch] failed to load @tauri-apps/plugin-http:", err);
    cachedTauriFetch = null;
  }
  return cachedTauriFetch;
}

/**
 * Fetch via Rust-side Tauri command when available (bypasses CORS + JS plugin
 * detection issues), then Tauri HTTP plugin, and finally browser fetch for
 * pure-web dev (`npm run dev:web`).
 */
export async function pipelineFetch(input, init = {}) {
  const url = typeof input === "string" ? input : String(input);
  const method = init?.method || "GET";
  const invoke = await getTauriInvoke();

  if (invoke) {
    try {
      const result = await invoke("http_request_text", {
        url,
        method,
        headersJson: JSON.stringify(normalizeHeaders(init.headers)),
        body: normalizeBody(init.body),
        timeoutMs: typeof init?.timeoutMs === "number" ? init.timeoutMs : undefined,
      });
      return new Response(result?.body ?? "", {
        status: Number(result?.status) || 200,
      });
    } catch (err) {
      if (!isMissingTauriCommand(err)) {
        console.error(`[pipelineFetch/tauri-invoke] ${method} ${url} ->`, err);
        throw buildTransportError(err, { url, method, label: "tauri-invoke" });
      }
      console.warn("[pipelineFetch] http_request_text unavailable, falling back:", err);
    }
  }

  const tauriFetch = await getTauriFetch();
  const label = tauriFetch ? "tauri-http" : "browser-fetch";
  try {
    const impl = tauriFetch || fetch;
    return await impl(input, init);
  } catch (err) {
    console.error(`[pipelineFetch/${label}] ${method} ${url} ->`, err);
    throw buildTransportError(err, { url, method, label });
  }
}

/**
 * Retry with exponential backoff. Honours AbortSignal between attempts.
 * @param {() => Promise<T>} fn
 * @param {{ retries?: number, signal?: AbortSignal, baseDelayMs?: number, maxDelayMs?: number, shouldRetry?: (err: unknown, attempt: number) => boolean }} opts
 * @returns {Promise<T>}
 * @template T
 */
export async function withRetry(fn, opts = {}) {
  const retries = opts.retries ?? 5;
  const baseDelay = opts.baseDelayMs ?? 2000;
  const maxDelay = opts.maxDelayMs ?? 30000;
  const signal = opts.signal;
  const shouldRetry = opts.shouldRetry || (() => true);

  let attempt = 0;
  while (true) {
    if (signal?.aborted) throw new DOMException("aborted", "AbortError");
    try {
      return await fn();
    } catch (err) {
      attempt += 1;
      if (signal?.aborted) throw new DOMException("aborted", "AbortError");
      if (attempt > retries || !shouldRetry(err, attempt)) {
        throw err;
      }
      const delay = Math.min(maxDelay, baseDelay * 2 ** (attempt - 1));
      await sleep(delay, signal);
    }
  }
}

export function sleep(ms, signal) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(new DOMException("aborted", "AbortError"));
      return;
    }
    const timer = setTimeout(() => {
      cleanup();
      resolve();
    }, ms);
    const onAbort = () => {
      cleanup();
      reject(new DOMException("aborted", "AbortError"));
    };
    function cleanup() {
      clearTimeout(timer);
      if (signal) signal.removeEventListener("abort", onAbort);
    }
    if (signal) signal.addEventListener("abort", onAbort);
  });
}

/**
 * Download binary bytes. Prefers a Rust-side `download_bytes` command so we
 * bypass plugin-http's `new Headers(responseHeaders)` path, which throws
 * `TypeError` when the server returns header values containing non-ASCII
 * bytes (observed from Volcano TOS CDN `Content-Disposition`).
 *
 * Falls back to `pipelineFetch` in pure-web dev or if the command is missing.
 *
 * @param {string} url
 * @param {AbortSignal} [signal]
 * @returns {Promise<Uint8Array>}
 */
export async function downloadBinary(url, signal) {
  if (signal?.aborted) throw new DOMException("aborted", "AbortError");
  const invoke = await getTauriInvoke();
  if (invoke) {
    try {
      const bytes = await invoke("download_bytes", { url });
      return bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    } catch (err) {
      console.error(`[downloadBinary/tauri] ${url} ->`, err);
      throw buildTransportError(err, { url, method: "GET", label: "download-bytes" });
    }
  }
  const response = await pipelineFetch(url, { method: "GET", signal });
  if (!response.ok) {
    throw new Error(`download HTTP ${response.status} for ${url}`);
  }
  const buf = await response.arrayBuffer();
  return new Uint8Array(buf);
}

/** Treat non-2xx as throw-with-body for clearer error reporting. */
export async function readJsonOrThrow(response, label) {
  const text = await response.text();
  if (!response.ok) {
    const trimmed = text.length > 500 ? `${text.slice(0, 500)}...` : text;
    throw new Error(`${label} HTTP ${response.status}: ${trimmed}`);
  }
  if (!text) return null;
  try {
    return JSON.parse(text);
  } catch {
    throw new Error(`${label} returned non-JSON response: ${text.slice(0, 200)}`);
  }
}
