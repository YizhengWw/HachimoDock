/**
 * [Input] Read DeviceContext.jsx source + runtime-import deriveCurrentDisplay.
 * [Output] Static + runtime Node coverage that the provider exposes the documented context shape, hydrates active channel state from the bridge profile, keeps USB and WiFi online state separate, polls USB status without owning serial auto-connect, offers a manual USB serial rescan action, and the pure derivation reflects the active desktop assignment.
 * [Pos] test node in ref/src/shell
 * [Sync] If this file changes, update `ref/src/shell/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import { deriveCurrentDisplay } from "./DeviceContext.pure.js";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "DeviceContext.jsx"), "utf8");

const APPEARANCES = [
  { id: "ap-a", name: "Terrier" },
  { id: "ap-b", name: "Westie" },
];
const AGENTS = [
  { id: "codex", label: "Codex", detected: true },
  { id: "claude-code", label: "Claude Code", detected: true },
];

test("deriveCurrentDisplay returns the active assignment with appearance + label", () => {
  const out = deriveCurrentDisplay(
    { codex: "ap-a" },
    new Set(["codex"]),
    APPEARANCES,
    AGENTS,
  );
  assert.equal(out.agentId, "codex");
  assert.equal(out.appearance?.id, "ap-a");
  assert.equal(out.channelLabel, "Codex");
});

test("deriveCurrentDisplay returns null appearance when map is empty", () => {
  const out = deriveCurrentDisplay({}, new Set(), APPEARANCES, AGENTS);
  assert.equal(out.agentId, "");
  assert.equal(out.appearance, null);
});

test("deriveCurrentDisplay falls back to the first mapped agent when no enabled set", () => {
  const out = deriveCurrentDisplay(
    { codex: "ap-b" },
    null,
    APPEARANCES,
    AGENTS,
  );
  assert.equal(out.agentId, "codex");
  assert.equal(out.appearance?.id, "ap-b");
});

test("provider source exposes useDeviceContext and DeviceContextProvider", () => {
  assert.match(source, /export function DeviceContextProvider\s*\(/);
  assert.match(source, /export function useDeviceContext\s*\(/);
});

test("provider centralizes the documented polling and bridge invocations", () => {
  // No new Tauri commands — strictly re-uses existing ones from the dashboards.
  for (const command of [
    "usb_get_status",
    "usb_scan_devices",
    "usb_connect",
    "check_device_availability",
    "load_bridge_profile",
    "load_device_bindings",
    "detect_local_agents",
  ]) {
    assert.match(source, new RegExp(`["']${command}["']`), `expected provider to invoke ${command}`);
  }
});

test("provider hydrates the active channel from bridge profile instead of stale localStorage only", () => {
  assert.match(source, /const\s+loadBridgeSelection\s*=\s*useCallback\(\s*async\s*\(\)\s*=>/);
  assert.match(source, /invoke\(["']load_bridge_profile["']\)/);
  assert.match(source, /profile\?\.selectedAgentId/);
  assert.match(source, /profile\?\.enabledAgents/);
  assert.match(source, /setEnabledAgents\(bridgeEnabled\)/);
  assert.match(source, /saveEnabledAgents\(bridgeEnabled\)/);
});

test("provider polls USB status but does not auto-connect inside the polling effect", () => {
  const pollEffect = source.match(/\/\/ --- USB status poll[\s\S]*?\n  \}, \[[^\]]*\]\);/);
  assert.ok(pollEffect, "expected USB status poll effect");
  assert.match(pollEffect[0], /usb_get_status/);
  assert.doesNotMatch(pollEffect[0], /["']usb_scan_devices["']/);
  assert.doesNotMatch(pollEffect[0], /["']usb_connect["']/);
  assert.doesNotMatch(source, /usb auto-connect failed/);
});

test("provider keeps WiFi availability separate while USB is connected", () => {
  assert.match(source, /const\s+\[wifiOnline,\s*setWifiOnline\]/);
  assert.match(source, /const\s+\[wifiBoardDeviceId,\s*setWifiBoardDeviceId\]/);
  const availabilityEffect = source.match(/\/\/ --- WiFi availability poll[\s\S]*?\n  \}, \[[^\]]*\]\);/);
  assert.ok(availabilityEffect, "expected WiFi availability poll effect");
  assert.match(availabilityEffect[0], /check_device_availability/);
  assert.doesNotMatch(availabilityEffect[0], /if\s*\(usb\.connected\)\s*\{/);
  assert.match(source, /const\s+deviceOnline\s*=\s*Boolean\(usb\.connected\s*\|\|\s*wifiOnline\)/);
});

test("provider exposes manual USB serial rescan and connect action", () => {
  assert.match(source, /const\s+rescanUsbDevices\s*=\s*useCallback\(\s*async\s*\(\)\s*=>/);
  assert.match(source, /invoke\(["']usb_scan_devices["']\)/);
  assert.match(source, /invoke\(["']usb_connect["'],\s*\{\s*portName:/);
  assert.match(source, /rescanUsbDevices/);
});

test("provider exposes the documented context shape fields", () => {
  for (const field of [
    "binding",
    "usb",
    "wifiOnline",
    "wifiBoardDeviceId",
    "deviceOnline",
    "onlineBoardDeviceId",
    "deviceConnected",
    "appearances",
    "agentAppearanceMap",
    "enabledAgents",
    "agentOptions",
    "currentDisplay",
    "currentComponent",
    "applyDesktopPet",
    "rescanUsbDevices",
    "refresh",
  ]) {
    assert.match(source, new RegExp(`\\b${field}\\b`), `expected context field ${field}`);
  }
});

test("provider reuses applyDesktopPetAssignment from lib (does not re-implement)", () => {
  assert.match(source, /from\s+["'][^"']*desktop-pet-assignment[^"']*["']/);
  assert.match(source, /applyDesktopPetAssignment\(/);
});

test("provider reads currentComponent from a stable localStorage key with null fallback", () => {
  assert.match(source, /pet-manager:active-component/);
});
