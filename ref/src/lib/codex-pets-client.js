/**
 * [Input] Tauri command invoker for Codex pet availability, scan, install, and import commands.
 * [Output] Cached client facade that avoids repeated local Codex pet scans unless explicitly refreshed.
 * [Pos] lib node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import { invoke as tauriInvoke } from "@tauri-apps/api/core";
import { createAsyncCache } from "./client-cache.js";

const CODEX_PETS_CACHE_TTL_MS = 30_000;

export function createCodexPetsClient({ invoke = tauriInvoke, ttlMs = CODEX_PETS_CACHE_TTL_MS } = {}) {
  const listCache = createAsyncCache({ ttlMs });

  return {
    checkFfmpegAvailable() {
      return invoke("check_ffmpeg_available");
    },

    listCodexPets(options = {}) {
      return listCache.get(() => invoke("list_codex_pets"), options);
    },

    invalidateCodexPetsCache() {
      listCache.invalidate();
    },

    async importCodexPet(petId) {
      return invoke("import_codex_pet", { petId });
    },

    async installCodexCommunityPet(petId) {
      const result = await invoke("install_codex_community_pet", { petId });
      listCache.invalidate();
      return result;
    },
  };
}

const defaultClient = createCodexPetsClient();

export const checkFfmpegAvailable = (...args) => defaultClient.checkFfmpegAvailable(...args);
export const listCodexPets = (...args) => defaultClient.listCodexPets(...args);
export const invalidateCodexPetsCache = (...args) => defaultClient.invalidateCodexPetsCache(...args);
export const importCodexPet = (...args) => defaultClient.importCodexPet(...args);
export const installCodexCommunityPet = (...args) => defaultClient.installCodexCommunityPet(...args);
