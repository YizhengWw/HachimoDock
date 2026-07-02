import { test, expect } from "@playwright/test";

/*
 * The desktop UI runs here as a plain Vite web build with no Tauri backend, so
 * invoke() calls reject and a couple of localhost fetches 404 — expected
 * browser-preview noise. These smoke tests only assert that the shell and each
 * main view render without an UNCAUGHT page error.
 */

async function waitForAppMounted(page) {
  await page.waitForFunction(
    () => (document.getElementById("root")?.textContent || "").trim().length > 20,
    { timeout: 15_000 },
  );
}

// The first-launch device guide auto-opens over the dashboard; dismiss it so it
// doesn't intercept sidebar clicks.
async function dismissDeviceGuide(page) {
  const skip = page.getByRole("button", { name: "跳过" });
  if (await skip.isVisible().catch(() => false)) {
    await skip.click().catch(() => {});
  }
}

test("app shell mounts with sidebar and dashboard", async ({ page }) => {
  const pageErrors = [];
  page.on("pageerror", (e) => pageErrors.push(e.message));

  await page.goto("/");
  await waitForAppMounted(page);

  await expect(page).toHaveTitle(/Pet-?Claw|Pet Manager/i);
  await expect(page.getByText("组件中心").first()).toBeVisible();
  expect(await page.locator("button").count()).toBeGreaterThan(0);
  expect(pageErrors, `uncaught page errors: ${pageErrors.join("; ")}`).toHaveLength(0);
});

test("each main tab renders without an uncaught error", async ({ page }) => {
  const pageErrors = [];
  page.on("pageerror", (e) => pageErrors.push(e.message));

  await page.goto("/");
  await waitForAppMounted(page);
  await dismissDeviceGuide(page);

  for (const tab of ["形象画廊", "组件中心", "设备"]) {
    await page.getByText(tab, { exact: true }).first().click();
    await expect(page.locator('[class*="page"]').first()).toBeVisible();
    await page.waitForTimeout(400);
  }

  expect(pageErrors, `uncaught page errors: ${pageErrors.join("; ")}`).toHaveLength(0);
});
