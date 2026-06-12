/**
 * [Input] Consume prototype runtime requirements defined by `ref/.folder.md`.
 * [Output] Provide Vite runtime configuration to downstream local development and build commands.
 * [Pos] config node in ref
 * [Sync] If this file changes, update this header and `ref/.folder.md`.
 */

import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  optimizeDeps: {
    exclude: ["@imgly/background-removal"],
  },
});
