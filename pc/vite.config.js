/**
 * [Input] Prototype runtime requirements with no bundled credentials.
 * [Output] Vite runtime configuration without embedded credentials.
 * [Pos] config node in ref
 * [Sync] If this file changes, update this header and `pc/.folder.md`.
 */

import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { componentUiMetadataPlugin } from "./scripts/component-ui-metadata.mjs";

export default defineConfig({
  build: process.env.PET_MANAGER_COMPOSER_DIAGNOSTIC === "1" ? {
    rollupOptions: { input: { diagnostic: "composer-diagnostic.html" } },
  } : {},
  server: {proxy: {'/community/':'http://127.0.0.1:4300'}},
  plugins: [componentUiMetadataPlugin(), react()],
});
