/**
 * [Input] Prototype runtime requirements with no bundled credentials.
 * [Output] Vite runtime configuration with an always-empty bundled credential constant.
 * [Pos] config node in ref
 * [Sync] If this file changes, update this header and `pc/.folder.md`.
 */

import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  define: {
    __PET_MANAGER_INTERNAL_CONTENT_API_KEY__: JSON.stringify(
      "",
    ),
  },
  plugins: [react()],
});
