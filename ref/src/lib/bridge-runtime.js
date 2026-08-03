/**
 * [Input] Tauri `ensure_bridge_runtime` plus repeated callers from setup, polling, and voice recovery.
 * [Output] One shared Bridge readiness gate that coalesces concurrent checks and briefly reuses a
 *          confirmed-ready result without caching failures.
 * [Pos] local runtime lifecycle helper in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import { invoke } from "@tauri-apps/api/core";

export const BRIDGE_READY_CACHE_MS = 5000;

export function createBridgeRuntimeEnsurer({
  invokeCommand = invoke,
  now = () => Date.now(),
  readyCacheMs = BRIDGE_READY_CACHE_MS,
} = {}) {
  let inFlight = null;
  let lastReadyResult = null;
  let lastReadyAt = 0;

  return function ensureBridgeRuntime({ forceRestart = false } = {}) {
    const requestedAt = now();
    if (!forceRestart && inFlight) return inFlight;
    if (
      !forceRestart
      && lastReadyResult?.running
      && requestedAt - lastReadyAt < readyCacheMs
    ) {
      return Promise.resolve(lastReadyResult);
    }

    let request;
    request = Promise.resolve()
      .then(() => invokeCommand("ensure_bridge_runtime", {
        input: { forceRestart },
      }))
      .then((result) => {
        if (result?.running) {
          lastReadyResult = result;
          lastReadyAt = now();
        } else {
          lastReadyResult = null;
          lastReadyAt = 0;
        }
        return result;
      })
      .catch((error) => {
        lastReadyResult = null;
        lastReadyAt = 0;
        throw error;
      })
      .finally(() => {
        if (inFlight === request) inFlight = null;
      });

    if (!forceRestart) inFlight = request;
    return request;
  };
}

export const ensureBridgeRuntime = createBridgeRuntimeEnsurer();
