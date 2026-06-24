/**
 * [Input] Read WifiApplyModal.jsx source.
 * [Output] Static Node coverage that the modal uses shared modal-card chrome
 *          and form controls, renders SSID/PSK inputs, invokes usb_apply_wifi
 *          with {ssid, psk} on submit, listens to the "usb-message" event
 *          filtered by topic === "apply-wifi-ack", renders the three stages
 *          (applying / connected / failed), and has a client-side timeout for
 *          missing terminal ACKs.
 * [Pos] test node in ref/src/dashboard
 * [Sync] If this file changes, update `ref/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "WifiApplyModal.jsx"), "utf8");

test("WifiApplyModal exports a default React component", () => {
  assert.match(source, /export default function WifiApplyModal\s*\(/);
});

test("Renders SSID and password inputs", () => {
  assert.match(source, /name=["']ssid["']|placeholder=["'][^"']*SSID[^"']*["']/i,
               "expected an SSID input");
  assert.match(source, /type=["']password["']/,
               "expected a password-typed input for PSK");
});

test("Uses shared modal-card chrome instead of an unstyled floating modal", () => {
  assert.match(source, /className=["']modal-card wifi-apply-modal["']/);
  assert.match(source, /className=["']modal-header["']/);
  assert.match(source, /className=["'][^"']*modal-body[^"']*["']/);
  assert.match(source, /className=["']modal-footer["']/);
  assert.doesNotMatch(source, /className=["']modal wifi-apply-modal["']/);
});

test("Uses shared form control primitives so fields stay aligned in the modal", () => {
  assert.match(source, /className=["']ui-field/);
  assert.match(source, /className=["']ui-field__label["']/);
  assert.match(source, /className=["']ui-control["']/);
  assert.doesNotMatch(source, /className=["']form-row["']/);
});

test("Invokes usb_apply_wifi Tauri command with ssid + psk", () => {
  assert.match(source, /invoke\(\s*["']usb_apply_wifi["']/);
  assert.match(source, /ssid[\s\S]{0,40}psk/);
});

test("Listens to usb-message events filtered by apply-wifi-ack", () => {
  assert.match(source, /listen\(\s*["']usb-message["']/);
  assert.match(source, /apply-wifi-ack/);
});

test("Renders three stages: applying, connected, failed", () => {
  assert.match(source, /applying/);
  assert.match(source, /connected/);
  assert.match(source, /failed/);
});

test("Times out locally when the board never returns a terminal ACK", () => {
  assert.match(source, /const\s+APPLY_TIMEOUT_MS\s*=\s*30_000/);
  assert.match(source, /window\.setTimeout/);
  assert.match(source, /window\.clearTimeout/);
  assert.match(source, /setError[\s\S]{0,120}["']timeout["']/);
});

test("Does not persist credentials outside component state", () => {
  /* No localStorage / sessionStorage writes touching ssid or password. */
  const lsWrites = source.match(/(localStorage|sessionStorage)\.setItem/g) || [];
  assert.equal(lsWrites.length, 0, "modal must not persist credentials to storage");
});

test("Restricts SSID and PSK to 64 chars (matches device-side limit)", () => {
  assert.match(source, /maxLength=\{?\s*64\s*\}?/);
});
