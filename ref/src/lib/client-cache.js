/**
 * [Input] Async client-side loader functions with optional force-refresh requests.
 * [Output] Small TTL cache primitive that reuses recent reads, exposes fresh cached values synchronously, and coalesces concurrent loads.
 * [Pos] lib node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

export function createAsyncCache({ ttlMs = 30_000, now = () => Date.now() } = {}) {
  let cachedValue;
  let hasCachedValue = false;
  let expiresAt = 0;
  let pending = null;

  return {
    async get(loader, { force = false } = {}) {
      const currentTime = now();
      if (!force && hasCachedValue && currentTime < expiresAt) {
        return cachedValue;
      }
      if (!force && pending) {
        return pending;
      }

      pending = Promise.resolve()
        .then(loader)
        .then((value) => {
          cachedValue = value;
          hasCachedValue = true;
          expiresAt = now() + ttlMs;
          return value;
        })
        .finally(() => {
          pending = null;
        });

      return pending;
    },

    peek() {
      const currentTime = now();
      return hasCachedValue && currentTime < expiresAt ? cachedValue : undefined;
    },

    invalidate() {
      cachedValue = undefined;
      hasCachedValue = false;
      expiresAt = 0;
      pending = null;
    },
  };
}
