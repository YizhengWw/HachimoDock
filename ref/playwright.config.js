import { defineConfig, devices } from "@playwright/test";

const PORT = 4173;

/**
 * E2E smoke tests for the desktop UI, run as a plain Vite web build (no Tauri
 * backend — device features fall back to the browser-preview adapters). These
 * are separate from the zero-install `npm test` unit suite; run with
 * `npm run test:e2e` (needs `npm install` + `npx playwright install chromium`).
 */
export default defineConfig({
  testDir: "./e2e",
  timeout: 30_000,
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  reporter: "list",
  use: {
    baseURL: `http://localhost:${PORT}`,
    trace: "on-first-retry",
  },
  projects: [
    {
      name: "chromium",
      use: {
        ...devices["Desktop Chrome"],
        // CI runs `npx playwright install chromium` and leaves this unset (default
        // browser resolution). Locally, point PW_EXECUTABLE_PATH at any installed
        // Chromium to skip a fresh browser download.
        launchOptions: { executablePath: process.env.PW_EXECUTABLE_PATH || undefined },
      },
    },
  ],
  webServer: {
    command: "npm run dev:web",
    url: `http://localhost:${PORT}`,
    reuseExistingServer: !process.env.CI,
    timeout: 60_000,
  },
});
