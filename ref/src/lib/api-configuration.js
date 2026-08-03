/**
 * [Input] Browser API-configuration changes plus provider/ASR status values.
 * [Output] Shared API configuration event, ASR resource catalog, and credential-readiness helpers used by settings and feature pages.
 * [Pos] config helper in ref/src/lib
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

export const API_CONFIGURATION_UPDATED_EVENT = "pet-manager:api-configuration-updated";

export const ASR_RESOURCE_OPTIONS = [
  { id: "volc.seedasr.sauc.duration", label: "豆包 ASR 2.0（推荐）" },
  { id: "volc.bigasr.sauc.duration", label: "豆包 ASR 1.0" },
];

export function providerCredentialsConfigured(providerId, config = {}) {
  if (providerId === "kling") {
    return Boolean(config.accessKey?.trim() && config.secretKey?.trim());
  }
  return Boolean(config.apiKey?.trim());
}
export function emitApiConfigurationUpdated(detail = {}) {
  if (typeof window === "undefined") return;
  window.dispatchEvent(new CustomEvent(API_CONFIGURATION_UPDATED_EVENT, { detail }));
}
