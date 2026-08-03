/**
 * [Input] The local Vite frontend, Playwright Chromium, and IMG.LY's official model CDN.
 * [Output] A strict-CSP browser smoke result covering model download, ONNX inference, and PNG output.
 * [Pos] Opt-in release-security smoke test for the avatar image preprocessing path.
 * [Sync] If this file changes, update scripts/.folder.md and ref/package.json.
 */

import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const scriptsDir = dirname(fileURLToPath(import.meta.url));
const refRoot = resolve(scriptsDir, "../ref");
const requireFromRef = createRequire(resolve(refRoot, "package.json"));
const { chromium } = requireFromRef("playwright");
const viteBin = resolve(refRoot, "node_modules/vite/bin/vite.js");
const port = Number(process.env.PET_MANAGER_SMOKE_PORT || 4287);
const origin = `http://127.0.0.1:${port}`;
const browserCandidates = process.platform === "win32" ? [
  process.env.PET_MANAGER_CHROMIUM_BIN,
  "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
  "C:/Program Files/Microsoft/Edge/Application/msedge.exe",
  "C:/Program Files/Google/Chrome/Application/chrome.exe",
  "C:/Program Files (x86)/Google/Chrome/Application/chrome.exe",
].filter(Boolean) : [];
const systemBrowser = browserCandidates.find((path) => existsSync(path));
const csp = [
  "default-src 'self'",
  "connect-src 'self' https://staticimgly.com blob: data:",
  "img-src 'self' data: blob:",
  "media-src 'self' data: blob:",
  "script-src 'self' 'wasm-unsafe-eval' blob:",
  "style-src 'self' 'unsafe-inline'",
  "worker-src 'self' blob:",
  "font-src 'self' data:",
].join("; ");

const serverOutput = [];
const server = spawn(
  process.execPath,
  [viteBin, "--host", "127.0.0.1", "--port", String(port), "--strictPort"],
  { cwd: refRoot, stdio: ["ignore", "pipe", "pipe"], windowsHide: true },
);
for (const stream of [server.stdout, server.stderr]) {
  stream.setEncoding("utf8");
  stream.on("data", (chunk) => {
    serverOutput.push(chunk);
    if (serverOutput.length > 40) serverOutput.shift();
  });
}

async function waitForServer() {
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    if (server.exitCode !== null) {
      throw new Error(`Vite exited before smoke test:\n${serverOutput.join("")}`);
    }
    try {
      const response = await fetch(origin);
      if (response.ok) return;
    } catch {
      // Server startup races are expected here.
    }
    await new Promise((resolveWait) => setTimeout(resolveWait, 250));
  }
  throw new Error(`Timed out waiting for ${origin}\n${serverOutput.join("")}`);
}

let browser;
try {
  await waitForServer();
  browser = await chromium.launch({
    headless: true,
    ...(systemBrowser ? { executablePath: systemBrowser } : {}),
  });
  const context = await browser.newContext();
  await context.route(`${origin}/`, async (route) => {
    await route.fulfill({
      status: 200,
      contentType: "text/html",
      headers: { "content-security-policy": csp },
      body: "<!doctype html><html><head><meta charset=\"UTF-8\"></head><body></body></html>",
    });
  });
  const page = await context.newPage();
  await page.addInitScript(() => {
    window.__petManagerCspViolations = [];
    document.addEventListener("securitypolicyviolation", (event) => {
      window.__petManagerCspViolations.push({
        directive: event.violatedDirective,
        blockedUri: event.blockedURI,
      });
    });
  });
  await page.goto(origin, { waitUntil: "domcontentloaded" });

  const result = await page.evaluate(async () => {
    const { processImageForPipeline } = await import(
      "/src/lib/avatar-pipeline/image-processing.js"
    );
    const canvas = document.createElement("canvas");
    canvas.width = 96;
    canvas.height = 96;
    const context2d = canvas.getContext("2d");
    context2d.fillStyle = "#f5f5f5";
    context2d.fillRect(0, 0, 96, 96);
    context2d.fillStyle = "#d9342b";
    context2d.beginPath();
    context2d.arc(48, 48, 28, 0, Math.PI * 2);
    context2d.fill();
    const input = await new Promise((resolveBlob) => canvas.toBlob(resolveBlob, "image/png"));
    const stages = [];
    const output = await processImageForPipeline(input, {
      maxDimension: 64,
      onProgress: (stage) => stages.push(stage),
    });
    return {
      width: output.width,
      height: output.height,
      byteLength: output.processedBytes.byteLength,
      signature: [...output.processedBytes.slice(0, 8)],
      stages,
      violations: window.__petManagerCspViolations,
    };
  });

  if (result.width !== 64 || result.height !== 48 || result.byteLength < 100) {
    throw new Error(`Unexpected background-removal output: ${JSON.stringify(result)}`);
  }
  if (result.signature.join(",") !== "137,80,78,71,13,10,26,10") {
    throw new Error(`Background-removal output is not PNG: ${JSON.stringify(result.signature)}`);
  }
  if (!result.stages.includes("removing_bg") || !result.stages.includes("done")) {
    throw new Error(`Background-removal stages were incomplete: ${JSON.stringify(result.stages)}`);
  }
  if (result.violations.length > 0) {
    throw new Error(`Strict CSP violations: ${JSON.stringify(result.violations)}`);
  }
  console.log(`Background-removal CSP smoke passed: ${result.width}x${result.height}, ${result.byteLength} bytes.`);
} finally {
  if (browser) await browser.close();
  server.kill();
}
