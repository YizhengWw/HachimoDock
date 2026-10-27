import { readFile } from "node:fs/promises";

// Match the native library's application_preview: controls only, never the full corpus.
export function componentUiMetadata(value) {
  const result = {...value};
  if (["knowledge", "calendar", "books"].includes(result.service)) result.content = [];
  return result;
}

export function componentUiMetadataPlugin() {
  return {
    name: "component-ui-metadata",
    enforce: "pre",
    async load(id) {
      if (!id.endsWith("/pc/application.json?component-ui")) return null;
      const value = JSON.parse(await readFile(id.split("?")[0], "utf8"));
      // Return JSON for Vite's JSON plugin, not executable application code.
      return JSON.stringify(componentUiMetadata(value));
    },
  };
}
