/**
 * [Input] Browser localStorage and the global text-rendering preference.
 * [Output] Persistent USB/Wi-Fi transport rendering defaults plus a change event consumed by the Tauri bootstrap.
 * [Pos] shared rendering preference helper in pc/src/lib
 */

export const TEXT_RENDERING_STORAGE_KEY = "hachimodock.text-rendering.v1";
export const TEXT_RENDERING_CHANGE_EVENT = "hachimodock:text-rendering-change";

export const DEFAULT_TEXT_RENDERING_SETTINGS = Object.freeze({
  usbApp: true,
  wifiApp: false,
});

function normalizeBoolean(value, fallback) {
  return typeof value === "boolean" ? value : fallback;
}

export function normalizeTextRenderingSettings(value = {}) {
  return {
    usbApp: normalizeBoolean(value.usbApp, DEFAULT_TEXT_RENDERING_SETTINGS.usbApp),
    wifiApp: normalizeBoolean(value.wifiApp, DEFAULT_TEXT_RENDERING_SETTINGS.wifiApp),
  };
}

export function loadTextRenderingSettings() {
  if (typeof window === "undefined" || !window.localStorage) {
    return { ...DEFAULT_TEXT_RENDERING_SETTINGS };
  }
  try {
    return normalizeTextRenderingSettings(
      JSON.parse(window.localStorage.getItem(TEXT_RENDERING_STORAGE_KEY) || "{}"),
    );
  } catch {
    return { ...DEFAULT_TEXT_RENDERING_SETTINGS };
  }
}

export function saveTextRenderingSettings(settings) {
  const next = normalizeTextRenderingSettings(settings);
  if (typeof window !== "undefined" && window.localStorage) {
    window.localStorage.setItem(TEXT_RENDERING_STORAGE_KEY, JSON.stringify(next));
  }
  if (typeof window !== "undefined") {
    window.dispatchEvent(new CustomEvent(TEXT_RENDERING_CHANGE_EVENT, { detail: next }));
  }
  return next;
}

export function toggleTextRenderingSetting(settings, key) {
  if (key !== "usbApp" && key !== "wifiApp") return normalizeTextRenderingSettings(settings);
  return saveTextRenderingSettings({
    ...normalizeTextRenderingSettings(settings),
    [key]: !normalizeTextRenderingSettings(settings)[key],
  });
}

/**
 * Resolve the rendering mode for a transport. Wi-Fi is intentionally stored
 * now but will only become active when the Wi-Fi transport lands.
 * @param {'usb'|'wifi'} transport - active transport.
 * @param {object} settings - persisted settings.
 * @returns {'app'|'client'} rendering owner.
 */
export function textRenderingModeForTransport(transport, settings = loadTextRenderingSettings()) {
  const normalized = normalizeTextRenderingSettings(settings);
  return transport === "wifi"
    ? (normalized.wifiApp ? "app" : "client")
    : (normalized.usbApp ? "app" : "client");
}
