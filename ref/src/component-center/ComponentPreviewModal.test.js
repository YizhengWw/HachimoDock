/**
 * [Input] Read ComponentPreviewModal.jsx source.
 * [Output] Static Node coverage: default export, prop signature, modal-backdrop, bindings list,
 *          replace warning, install-button disabled states.
 * [Pos] test node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "ComponentPreviewModal.jsx"), "utf8");

// 1. Default export
test("ComponentPreviewModal has a default export function named ComponentPreviewModal", () => {
  assert.match(source, /export default function ComponentPreviewModal\s*\(/);
});

// 2. Accepts all 6 props
test("ComponentPreviewModal accepts component, isDraft, currentComponent, deviceConnected, installing, onInstall, onClose props", () => {
  assert.match(source, /\bcomponent\b/);
  assert.match(source, /\bisDraft\b/);
  assert.match(source, /\bcurrentComponent\b/);
  assert.match(source, /\bdeviceConnected\b/);
  assert.match(source, /\binstalling\b/);
  assert.match(source, /\bonInstall\b/);
  assert.match(source, /\bonClose\b/);
});

// 3. Renders modal-backdrop
test("ComponentPreviewModal renders an element with class modal-backdrop", () => {
  assert.match(source, /className="modal-backdrop"/);
});

// 4. Renders bindings list when component.defaultBindings present
test("ComponentPreviewModal renders button-bindings section and maps over component.defaultBindings", () => {
  assert.match(source, /component\.defaultBindings/);
  assert.match(source, /component-preview-modal__bindings/);
  assert.match(source, /按钮映射/);
  assert.match(source, /\.defaultBindings\.filter\(isRoutedWidgetBinding\)\.map/);
});

test("ComponentPreviewModal renders human-readable control gesture labels", () => {
  assert.match(source, /formatBindingControl/);
  assert.match(source, /isRoutedWidgetBinding/);
  assert.match(source, /component-preview-modal__binding-control/);
});

// 5. Replace warning shown when currentComponent.id !== component.id
test("ComponentPreviewModal renders replace warning when currentComponent.id !== component.id", () => {
  assert.match(source, /currentComponent\.id !== component\.id/);
  assert.match(source, /安装后将替换当前的/);
});

// 6. Install button disabled when not connected or installing
test("ComponentPreviewModal disables install button when not deviceConnected or when installing", () => {
  assert.match(source, /disabled=\{installing \|\| !deviceConnected\}/);
});
