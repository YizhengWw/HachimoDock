/**
 * [Input] Read ComponentPreviewModal.jsx source.
 * [Output] Static Node coverage: default export, explicit game/tool type, button editor,
 *          component-scope/conflict guidance, single-slot replacement warning, current-device removal,
 *          dynamic global-return guidance, and install-button disabled states.
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
const styles = readFileSync(join(here, "..", "styles.css"), "utf8");

// 1. Default export
test("ComponentPreviewModal has a default export function named ComponentPreviewModal", () => {
  assert.match(source, /export default function ComponentPreviewModal\s*\(/);
});

test("ComponentPreviewModal renders every action through the shared Button primitive", () => {
  assert.match(source, /import Button from "\.\.\/shell\/Button"/);
  assert.equal((source.match(/<Button\b/g) || []).length, 6);
  assert.doesNotMatch(source, /<button\b/);
  assert.match(source, /variant="icon"/);
  assert.match(source, /variant="ghost"/);
  assert.match(source, /variant="danger"/);
  assert.match(source, /variant="secondary"/);
  assert.match(source, /variant="primary"/);
});

// 2. Accepts install + button-editor props
test("ComponentPreviewModal accepts component, install, and per-component button props", () => {
  assert.match(source, /\bcomponent\b/);
  assert.match(source, /\bkind\b/);
  assert.match(source, /\bisLocal\b/);
  assert.match(source, /\bisInstalled\b/);
  assert.match(source, /\bcurrentComponent\b/);
  assert.match(source, /\bdeviceConnected\b/);
  assert.match(source, /\binstalling\b/);
  assert.match(source, /\bonInstall\b/);
  assert.match(source, /\bonRemove\b/);
  assert.match(source, /\bonDelete\b/);
  assert.match(source, /\bonClose\b/);
  assert.match(source, /\bbindings\b/);
  assert.match(source, /\bglobalExitControl\b/);
  assert.match(source, /\bonBindingChange\b/);
  assert.match(source, /\bcomponentButtonsWillApply\b/);
  assert.match(source, /\binstallBlockedReason\b/);
});

// 3. Renders modal-backdrop
test("ComponentPreviewModal renders an element with class modal-backdrop", () => {
  assert.match(source, /className="modal-backdrop"/);
  assert.match(source, /role="dialog"/);
  assert.match(source, /aria-modal="true"/);
  assert.match(source, /aria-labelledby="component-preview-modal-title"/);
});

test("ComponentPreviewModal separates overview, configuration, impact, and confirmation regions", () => {
  assert.match(source, /component-preview-modal__overview/);
  assert.match(source, /component-preview-modal__summary/);
  assert.match(source, /component-preview-modal__workspace/);
  assert.match(source, /component-preview-modal__impact-title/);
  assert.match(source, /component-preview-modal__footer-copy/);
  assert.match(source, /component-preview-modal__footer-actions/);
  assert.match(source, /组件预览与说明/);
  assert.match(source, /同步影响/);
});

test("ComponentPreviewModal layout has desktop and compact responsive contracts", () => {
  assert.match(
    styles,
    /\.component-preview-modal__layout\s*\{[^}]*grid-template-columns:\s*minmax\(300px,\s*360px\)\s*minmax\(0,\s*1fr\)/s,
  );
  assert.match(styles, /\.component-preview-modal__overview\s*\{[^}]*position:\s*sticky/s);
  assert.match(styles, /@media \(max-width:\s*840px\)[\s\S]*?\.component-preview-modal__layout\s*\{[^}]*grid-template-columns:\s*minmax\(0,\s*1fr\)/);
  assert.match(styles, /@media \(max-width:\s*680px\)[\s\S]*?\.component-preview-modal__bindings ul\s*\{[^}]*max-height:\s*none[^}]*overflow:\s*visible/s);
  assert.match(styles, /@media \(max-width:\s*680px\)[\s\S]*?\.component-preview-modal__footer-actions\s*\{[^}]*grid-template-columns:\s*1fr 1fr/);
});

// 4. Renders editable bindings list
test("ComponentPreviewModal renders a per-component button editor", () => {
  assert.match(source, /component-preview-modal__bindings/);
  assert.match(source, /这个组件的按钮/);
  assert.match(source, /bindings\.map/);
  assert.match(source, /<select/);
  assert.match(source, /onBindingChange/);
  assert.match(source, /已占用/);
  assert.match(source, /恢复默认/);
  assert.match(source, /SW1、SW2、SW3 当前映射/);
  assert.match(source, /component-preview-modal__switch-strip/);
  assert.match(source, /动作逻辑不变/);
});

test("ComponentPreviewModal becomes a save-and-downlink editor for the running component", () => {
  assert.match(source, /isUpdatingCurrent/);
  assert.match(
    source,
    /typeof isCurrent === "boolean"[\s\S]*\? isCurrent[\s\S]*currentComponent\?\.id/,
  );
  assert.match(source, /调整小游戏按钮/);
  assert.match(source, /调整工具组件/);
  assert.match(source, /保存并同步/);
});

test("ComponentPreviewModal explains resync for a package already stored on the board", () => {
  assert.match(source, /已同步组件详情/);
  assert.match(source, /这个组件已同步到设备/);
  assert.match(source, /重新同步组件配置和按钮/);
  assert.match(source, /component\.isDeviceOnly \? "设备包"/);
  assert.match(source, /!isDeviceOnly && typeof onInstall === "function"/);
});

test("ComponentPreviewModal turns unknown device packages into read-only details", () => {
  assert.match(source, /设备包详情/);
  assert.match(source, /设备包信息/);
  assert.match(source, /本机没有对应安装源/);
  assert.match(source, /isDeviceOnly && typeof onInstall/);
});

test("ComponentPreviewModal uses explicit kind metadata and labels game/tool components", () => {
  assert.match(source, /resolveComponentKind\(kind, component\.gameType\)/);
  assert.match(source, /componentKindLabel\(resolvedKind\)/);
  assert.match(source, /component-preview-modal__kind/);
});

test("ComponentPreviewModal offers device removal for enabled items and exact device-only packages", () => {
  assert.match(source, /\(isInstalled \|\| isDeviceOnly\) && onRemove/);
  assert.match(source, /component-preview-modal__action--remove/);
  assert.match(source, /onClick=\{onRemove\}/);
  assert.match(source, /从设备移除/);
  assert.match(source, /<Unplug/);
});

test("ComponentPreviewModal offers PC or dual deletion for every formal local component", () => {
  assert.match(source, /isLocal && typeof onDelete === "function"/);
  assert.match(source, /component-preview-modal__action--delete/);
  assert.match(source, /onClick=\{onDelete\}/);
  assert.match(source, /isInstalled \? "从电脑和设备删除" : "从电脑删除"/);
  assert.match(source, /<Trash2/);
});

// 5. Multi-slot sync has no ambiguous current-component replacement copy.
test("ComponentPreviewModal does not describe multi-slot sync as replacing a current component", () => {
  assert.doesNotMatch(source, /安装后将替换当前的/);
});

test("ComponentPreviewModal explains destructive single-slot replacement in the final confirmation", () => {
  assert.match(source, /singleSlotReplacement/);
  assert.match(source, /当前设备是单槽模式/);
  assert.match(source, /旧组件不会保留在板端/);
  assert.match(source, /替换并启用/);
});

test("ComponentPreviewModal traps keyboard focus, closes on Escape, and restores the opener", () => {
  assert.match(source, /dialogRef/);
  assert.match(source, /returnFocusRef/);
  assert.match(source, /event\.key === "Escape"/);
  assert.match(source, /event\.key !== "Tab"/);
  assert.match(source, /previous\?\.isConnected/);
});

test("ComponentPreviewModal explains component exit follows the global mapping", () => {
  assert.match(source, /componentButtonsWillApply/);
  assert.match(source, /组件按钮仅包含游戏或工具动作/);
  assert.match(source, /退出始终跟随设备全局设置/);
  assert.match(source, /当前退出键是 \{globalExitControl\}/);
  assert.match(source, /component-preview-modal__impact/);
  assert.match(source, /同步影响/);
});

// 6. Install button disabled when not connected, installing, or conflicting
test("ComponentPreviewModal guards install until button conflicts and firmware support are resolved", () => {
  assert.match(source, /Boolean\(bindingConflict\)/);
  assert.match(source, /Boolean\(installBlockedReason\)/);
  assert.match(source, /loading=\{installing\}/);
  assert.match(source, /loadingLabel="同步中…"/);
  assert.match(source, /暂时无法安装/);
});
