/**
 * [Input] ComponentCenter.jsx, App.jsx, and fixtures.js component-center source.
 * [Output] Static Node coverage for the type-filtered library + modal layout.
 *          Tests assert CandidateCard / ComponentPreviewModal wiring,
 *          session-scoped device-inventory caching and formal-library file watching, while verifying
 *          first-visit component onboarding,
 *          the featured 双键接球 builtin first, newest-first user components next, complete built-in Flappy package wiring,
 *          per-component buttons.json, editable component bindings, explicit component-scope guidance,
 *          P4 button-map downlink, semantic enabled state, card-owned device sync/removal,
 *          device-first dual deletion, exact target identity, focus-safe confirmation,
 *          critical install pipelines, and
 *          bridge-authoritative live Agent following plus cross-page contracts remain.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));

function readSource(fileName) {
  return readFileSync(join(srcDir, fileName), "utf8");
}

// ── Unified library + modal layout ────────────────────────────────────────────

test("ComponentCenter uses one library and removes the separate board overview", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /id="component-library"/);
  assert.match(component, /组件库卡片会直接显示设备同步状态/);
  assert.match(component, /component-library-filters/);
  assert.match(component, /label: "全部"/);
  assert.match(component, /label: "小游戏"/);
  assert.match(component, /label: "工具"/);
  assert.doesNotMatch(component, /DeviceComponentOverview/);
  assert.doesNotMatch(component, /上方查看板端组件，下方浏览完整组件库/);
  assert.doesNotMatch(component, /component-library-section__header/);
  assert.doesNotMatch(component, /component-library-search/);
  assert.doesNotMatch(component, /libraryQuery|normalizedLibraryQuery/);
  assert.doesNotMatch(component, /内置与正式本地组件/);
  assert.doesNotMatch(component, /正式本地 \{localComponents\.length\}/);
});

test("component center exposes a first-visit guide for direct card sync", () => {
  const component = readSource("ComponentCenter.jsx");

  assert.match(component, /usePageOnboarding\(ONBOARDING_PAGE_IDS\.COMPONENT_CENTER\)/);
  assert.match(component, /help=\{onboarding\.show\}/);
  assert.match(component, /<PageOnboardingModal/);
  assert.match(component, /title="添加或创建小组件，只要三步"/);
  assert.match(component, /浏览或创建/);
  assert.match(component, /预览与配置/);
  assert.match(component, /直接同步/);
  assert.match(component, /已同步组件可从同一张卡片移除/);
  assert.match(component, /label: "浏览组件库"/);
  assert.match(component, /label: "创建组件"/);
  assert.match(component, /focusComponentLibrary\(\)/);
  assert.match(component, /setCreateDrawerOpen\(true\)/);
});

test("each component card owns its immediate sync or device-removal action", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /invoke\("list_device_widgets"/);
  assert.match(component, /setDeviceInventory\(nextInventory\)/);
  assert.match(component, /onDeviceAction=\{\(\) => \{/);
  assert.match(component, /if \(isInstalled\) \{[\s\S]*requestRemoveInventoryItem/);
  assert.match(component, /else \{[\s\S]*setPreviewComponent\(item\)/);
  assert.match(component, /onInstall=\{[\s\S]*syncSelectedComponent\(previewComponent\)/);
  assert.doesNotMatch(component, /pendingDeviceInstalls|pendingDeviceRemovalIds/);
  assert.doesNotMatch(component, /syncPendingDeviceChanges|discardPendingDeviceChanges/);
});

test("direct sync preflights one component and respects board capacity", () => {
  const component = readSource("ComponentCenter.jsx");
  const syncBody = component.slice(
    component.indexOf("async function syncSelectedComponent"),
    component.indexOf("async function installSelectedComponent"),
  );
  assert.match(syncBody, /localInstallBlockedReason\(component\)/);
  assert.match(syncBody, /gameInstallBlockedReason\(component, usb\)/);
  assert.match(syncBody, /deviceInventory\.items\.length >= maxInstalled/);
  assert.match(syncBody, /await installSelectedComponent\(component\)/);
  assert.match(syncBody, /请先在任一已同步组件卡片上点击移除/);
});

test("custom deletion is device-first and keeps the PC source after a failed ACK", () => {
  const component = readSource("ComponentCenter.jsx");
  const actionBody = component.slice(
    component.indexOf("async function confirmComponentAction"),
    component.indexOf("function resolveControlOption"),
  );
  const removeIndex = actionBody.indexOf('invoke("remove_widget_from_device"');
  const deleteIndex = actionBody.indexOf('invoke("delete_component_from_library"');
  assert.ok(removeIndex >= 0);
  assert.ok(deleteIndex > removeIndex);
  assert.match(component, /A failed board ACK leaves the local[\s\S]*package intact/);
  assert.match(component, /requestDeleteLibraryComponent\(item, isInstalled\)/);
});

test("direct install retains per-component OTA progress and errors", () => {
  const component = readSource("ComponentCenter.jsx");
  const installBody = component.slice(
    component.indexOf("async function performOtaInstall"),
    component.indexOf("async function installBuiltinToDevice"),
  );

  assert.match(installBody, /setOtaPhase\("installing"\)/);
  assert.match(installBody, /setOtaPhase\("success"\)/);
  assert.match(installBody, /setOtaPhase\("error"\)/);
  assert.doesNotMatch(installBody, /batchSync/);
});

test("board inventory is fetched once per App session and reused across page visits", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /const SESSION_DEVICE_INVENTORY_CACHE = new Map\(\)/);
  assert.match(component, /const SESSION_DEVICE_INVENTORY_REQUESTS = new Map\(\)/);
  assert.match(component, /function readSessionDeviceInventory\(target\)/);
  assert.match(component, /function writeSessionDeviceInventory\(target, inventory\)/);
  assert.match(component, /function requestSessionDeviceInventory\(target\)/);
  assert.match(
    component,
    /useState\(\s*\(\) => readSessionDeviceInventory\(liveInventoryTarget\) \|\| EMPTY_DEVICE_INVENTORY/,
  );
  assert.match(
    component,
    /const cachedInventory = readSessionDeviceInventory\(liveInventoryTarget\);[\s\S]*?setDeviceInventory\(cachedInventory\);[\s\S]*?refreshDeviceInventory\(\);/,
  );
  const inventoryEffect = component.slice(
    component.indexOf("const cachedInventory = readSessionDeviceInventory"),
    component.indexOf("window.localStorage.setItem", component.indexOf("const cachedInventory")),
  );
  assert.doesNotMatch(inventoryEffect, /setInterval|30000/);
});

test("ComponentCenter renders the component library grid using CandidateCard", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /import\s+CandidateCard/);
  assert.match(component, /component-library-grid/);
  assert.match(component, /<CandidateCard/);
});

test("ComponentCenter opens ComponentPreviewModal on candidate click (transient selection)", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /import\s+ComponentPreviewModal/);
  assert.match(component, /previewComponent/);
  assert.match(component, /<ComponentPreviewModal/);
});

test("device-only inventory packages join the grid as removable read-only cards", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /isDeviceOnly:\s*true/);
  assert.match(component, /本机没有这个组件的安装源/);
  assert.match(
    component,
    /previewComponent\.isDeviceOnly[\s\S]*\? undefined[\s\S]*syncSelectedComponent/,
  );
  assert.match(component, /buildUnknownInventoryComponent/);
  assert.match(component, /return \[\.\.\.catalogItems, \.\.\.deviceOnlyItems\]/);
});

test("preview modal confirms and immediately syncs the selected component", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /currentComponent=\{currentFullComponent\}/);
  assert.match(component, /syncSelectedComponent\(previewComponent\)/);
  assert.doesNotMatch(component, /pendingSync=\{Boolean/);
  assert.doesNotMatch(component, /plannedOverviewItems/);
});

test("active component is persisted only after the package install succeeds", () => {
  const component = readSource("ComponentCenter.jsx");
  const successIndex = component.indexOf("componentPackageInstalled = true");
  const persistIndex = component.indexOf(
    "markComponentInstalled(component, installTarget)",
    successIndex,
  );
  assert.ok(successIndex >= 0);
  assert.ok(persistIndex > successIndex);
  const selectionBody = component.slice(
    component.indexOf("async function installSelectedComponent"),
    component.indexOf("function markComponentInstalled"),
  );
  assert.doesNotMatch(selectionBody, /setActiveNegativeScreenId|localStorage\.setItem/);
  assert.match(component, /组件已安装，本地状态更新失败/);
});

test("per-component button choices persist and the preview editor shows resolved mappings", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /COMPONENT_BUTTON_OVERRIDES_STORAGE_KEY/);
  assert.match(component, /useState\(loadComponentButtonOverrides\)/);
  assert.match(
    component,
    /COMPONENT_BUTTON_OVERRIDES_STORAGE_KEY,[\s\S]*JSON\.stringify\(bindingOverrides\)/,
  );
  assert.match(
    component,
    /bindings=\{resolveComponentBindings\(previewComponent\)\}/,
  );
  assert.match(component, /onClick=\{\(\) => setPreviewComponent\(item\)\}/);
});

test("component button editor only offers screen gestures to a touch-ready device", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /function deviceTouchReady\(usb\)/);
  assert.match(component, /option\.event\.startsWith\("screen\."\)/);
  assert.match(component, /当前设备未报告触屏可用/);
  assert.match(component, /请改为 SW1\/SW2\/SW3 或旋钮/);
});

test("USB preflight trusts the shared DeviceContext USB state", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /if \(usb\.connected && boardDeviceId\)/);
  assert.match(component, /const deviceConnected = Boolean\(liveInventoryTarget\)/);
  assert.match(component, /if \(!status\?\.connected && !deviceConnected\)/);
});

test("Library leads with the featured builtin, then local components, while live board inventory drives state", () => {
  const component = readSource("ComponentCenter.jsx");
  const catalogBody = component.slice(
    component.indexOf("const catalogItems = useMemo"),
    component.indexOf("const libraryItems = useMemo"),
  );
  assert.match(catalogBody, /FEATURED_BUILTIN_COMPONENT_ID/);
  assert.match(catalogBody, /FEATURED_BUILTIN_VERSION_HASH/);
  assert.match(
    catalogBody,
    /\.\.\.featuredBuiltins,[\s\S]*\.\.\.sortComponentsByCreatedAt\(publishedItems\),[\s\S]*\.\.\.remainingBuiltins/,
  );
  assert.match(
    component,
    /deviceInventory\.activeWidgetId === item\.id[\s\S]*componentSourceKey\(item\) === enabledComponentSourceKey/,
  );
  assert.match(component, /const isInstalled = deviceInventory\.freshness === "live" && installedIds\.has\(item\.id\)/);
  assert.match(component, /isEnabled=\{isEnabled\}/);
  assert.match(component, /isInstalled=\{isInstalled\}/);
  assert.match(component, /kind=\{resolveComponentKind\(item\.kind, item\.gameType\)\}/);
});

test("formal local cards use manifest descriptions instead of exposing filesystem paths", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /component\.description/);
  assert.match(component, /自定义组件 · 可预览后添加到负一屏/);
  assert.doesNotMatch(component, /goal:\s*`[^`]*\$\{entry\.path/);
});

test("Library exposes compact game/tool filters without search chrome", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /component-library/);
  assert.match(component, /setLibraryKind/);
  assert.match(component, /小游戏/);
  assert.match(component, /工具/);
  assert.equal(component.includes('type="search"'), false);
  assert.equal(component.includes("setLibraryQuery"), false);
  assert.equal(component.includes("component-library-search"), false);
});

test("ComponentCenter keeps the 创建组件 actions button + drawer wiring", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /创建组件/);
  assert.match(component, /CreateComponentDrawer/);
  assert.match(component, /setCreateDrawerOpen/);
});

test("ComponentCenter writes target-scoped active state on successful install (driving ContextRail)", () => {
  const component = readSource("ComponentCenter.jsx");
  const store = readSource("lib/active-component-store.js");
  assert.match(component, /writeActiveComponentForTarget\(component, target\)/);
  assert.match(store, /ACTIVE_COMPONENT_STORAGE_KEY = "pet-manager:active-component"/);
  assert.match(store, /activeByTarget/);
  assert.match(component, /new Event\("storage"\)/);
});

test("installing a widget no longer applies a red-button preset", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.doesNotMatch(component, /applyRecommendedButtonConfigForWidget/);
  assert.doesNotMatch(component, /已应用推荐按键/);
  assert.doesNotMatch(component, /顶部红钮交给当前组件/);
});

test("ComponentCenter uses useToast for success/error notifications", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /useToast\(/);
  assert.match(component, /push\(\s*\{[\s\S]*?tone:\s*"success"/);
  assert.match(component, /push\(\s*\{[\s\S]*?tone:\s*"error"/);
});

test("Existing install pipeline preserved (installBuiltinToDevice, installClawpkgFromPath)", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /installBuiltinToDevice/);
  assert.match(component, /installClawpkgFromPath/);
});

test("builtin component install resolves bundled resources instead of requiring ~/.openclaw", () => {
  const component = readSource("ComponentCenter.jsx");

  assert.match(component, /invoke\("resolve_builtin_clawpkg_path"/);
  assert.match(component, /const clawpkgPath = await invoke\("resolve_builtin_clawpkg_path"/);
  assert.doesNotMatch(component, /homeDir\(\)/);
  assert.doesNotMatch(component, /\.openclaw\/builtin-clawpkgs|\.openclaw\\builtin-clawpkgs/);
});

test("Tauri bundle includes built-in clawpkg resources", () => {
  const tauriConfig = JSON.parse(readFileSync(join(srcDir, "../src-tauri/tauri.conf.json"), "utf8"));
  assert.equal(
    tauriConfig.bundle.resources["../builtin-clawpkgs"],
    "builtin-clawpkgs",
  );
});

test("OTA, single-slot replacement guidance, and device-action confirmation remain while prompt modal is removed", () => {
  const component = readSource("ComponentCenter.jsx");
  // The remaining modal sections handle USB OTA progress and draft/remove actions;
  // single-slot replacement is explained in the final preview confirmation.
  // Prompt generation now launches the agent directly without showing a copy-prompt dialog.
  assert.match(component, /ota-modal|otaTargetName/);
  assert.match(component, /pendingComponentAction|componentActionPending/);
  assert.match(component, /previewReplacesSingleSlot/);
  assert.match(component, /singleSlotReplacement=\{previewReplacesSingleSlot\}/);
  assert.match(component, /role="alertdialog"/);
  assert.match(component, /系统会先从当前设备删除组件/);
  assert.match(component, /只有设备确认成功后，才会继续删除电脑中的组件源/);
  assert.doesNotMatch(component, /aria-label="生成组件 prompt"/);
  assert.doesNotMatch(component, /component-generated-prompt/);
  assert.doesNotMatch(component, /当前渠道：/);
  assert.doesNotMatch(component, /MagicMirror 模块会先转换成 OpenClaw 组件/);
});

// ── Unchanged structural assertions ───────────────────────────────────────────

test("component center uses PageShell and shell components", () => {
  const component = readSource("ComponentCenter.jsx");
  const app = readSource("App.jsx");

  // Shell consumption
  assert.match(component, /from "\.\/shell\/PageShell\.jsx?"/);
  assert.match(component, /from "\.\/shell\/ToastStack\.jsx?"/);
  assert.match(component, /from "\.\/shell\/DeviceContext\.jsx?"/);
  assert.match(component, /<PageShell\b/);
  assert.match(component, /title="组件中心"/);
  assert.match(component, /useToast\(/);
  assert.match(component, /useDeviceContext\(/);

  // Action buttons in PageShell actions
  assert.match(component, /actions=\{/);
  assert.match(component, /刷新组件库/);
  assert.match(component, /setCreateDrawerOpen\(true\)/);

  // App.jsx wiring
  assert.match(app, /import ComponentCenter from "\.\/ComponentCenter"/);
  assert.match(app, /title="组件中心"/);
  assert.match(app, /<ComponentCenter \/>/);
});

test("component center preserves install, formal-library delete, and device-remove pipelines", () => {
  const component = readSource("ComponentCenter.jsx");

  // localStorage cross-page contract
  assert.match(component, /writeActiveComponentForTarget/);
  assert.match(component, /removeActiveComponentForTarget/);
  assert.match(component, /new Event\(\s*"storage"\s*\)/);

  // Toast notifications
  assert.match(component, /push\(\{[\s\S]*tone:\s*"success"/);
  assert.match(component, /push\(\{[\s\S]*tone:\s*"error"/);
  assert.doesNotMatch(component, /setClawpkgImportError\(/);
  assert.doesNotMatch(component, /setSkillInstallError\(/);
  assert.doesNotMatch(component, /component-tool-error/);

  // USB context (no local useState for usb)
  assert.doesNotMatch(component, /const \[usbConnected, setUsbConnected\] = useState/);
  assert.match(component, /usb\.connected/);

  // Core logic functions preserved
  assert.match(component, /function installSelectedComponent/);
  assert.match(component, /installBuiltinToDevice/);
  assert.match(component, /installClawpkgFromPath/);
  assert.match(component, /install_clawpkg_over_usb/);
  assert.match(component, /install_widget_skill/);
  assert.match(component, /delete_component_from_library/);
  assert.match(component, /remove_widget_from_device/);

  // Binding resolution preserved
  assert.match(component, /COMPONENT_CONTROL_OPTIONS/);
  assert.match(component, /button-config\.js/);
  assert.match(
    component,
    /const \[bindingOverrides, setBindingOverrides\] = useState\(loadComponentButtonOverrides\)/,
  );
  assert.match(component, /buildBindingOverridesForInstall/);
  assert.match(component, /isRoutedWidgetBinding/);
  assert.doesNotMatch(component, /applyComponentButtonConfig/);
  assert.match(readFileSync(join(srcDir, "component-center/button-config.js"), "utf8"), /miniapp_action/);

  // Formal local library pipeline
  assert.match(component, /localComponents\.map|list_component_library/);
  assert.match(component, /pendingComponentAction/);
  assert.match(component, /confirmComponentAction/);
  assert.match(component, /refreshComponentLibrary/);

  // Modals and explicit single-slot replacement guidance preserved
  assert.match(component, /component-replace-modal/);
  assert.match(component, /previewReplacesSingleSlot/);
  assert.doesNotMatch(component, /showReplaceConfirm/);
  assert.doesNotMatch(component, /component-generated-prompt/);
  assert.match(component, /ota-modal|otaTargetName/);
});

test("library cards expose direct device removal and device-first dual deletion", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /item\.isLocal[\s\S]*requestDeleteLibraryComponent\(item, isInstalled\)/);
  assert.doesNotMatch(component, /item\.isLocal && !isInstalled/);
  assert.match(component, /previewComponent\.isLocal[\s\S]*requestDeleteLibraryComponent\(component, previewIsInstalled\)/);
  assert.match(component, /onDeviceAction=\{\(\) => \{/);
  assert.match(component, /requestRemoveInventoryItem/);
  assert.match(component, /从电脑和设备删除/);
  assert.match(component, /type: "delete-library"[\s\S]*installedOnDevice/);
  assert.match(
    component,
    /async function confirmComponentAction[\s\S]*invoke\("remove_widget_from_device"[\s\S]*invoke\("delete_component_from_library"/,
  );
});

test("device removal clears active state only after the backend confirms success", () => {
  const component = readSource("ComponentCenter.jsx");
  const actionBody = component.slice(
    component.indexOf("async function confirmComponentAction"),
    component.indexOf("function resolveControlOption"),
  );
  const invokeIndex = actionBody.indexOf('invoke("remove_widget_from_device"');
  const rejectIndex = actionBody.indexOf("deviceResult.ok === false", invokeIndex);
  const clearIndex = actionBody.indexOf("clearActiveComponentState(target)", invokeIndex);
  assert.ok(invokeIndex >= 0);
  assert.ok(rejectIndex > invokeIndex);
  assert.ok(clearIndex > rejectIndex);
  assert.match(
    actionBody,
    /invoke\("remove_widget_from_device",\s*\{[\s\S]*componentId: component\.id,[\s\S]*transport: target\.transport,[\s\S]*boardDeviceId: target\.boardDeviceId \|\| "",[\s\S]*sshHost: target\.sshHost \|\| ""/,
  );
  assert.doesNotMatch(actionBody, /usb_set_screen_page/);

  const clearBody = component.slice(
    component.indexOf("function clearActiveComponentState"),
    component.indexOf("function clearComponentBindingOverrides"),
  );
  assert.match(clearBody, /setActiveComponentRecord\(null\)/);
  assert.match(clearBody, /removeActiveComponentForTarget\(target\)/);
  assert.match(clearBody, /window\.dispatchEvent\(new Event\("storage"\)\)/);
});

test("deleting a formal local component also clears stale per-component button overrides", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /function clearComponentBindingOverrides\(component\)/);
  assert.match(component, /const keyPrefix = `\$\{componentSourceKey\(component\)\}:`/);
  assert.match(component, /!key\.startsWith\(keyPrefix\)/);
  assert.match(
    component,
    /invoke\("delete_component_from_library"[\s\S]*?clearComponentBindingOverrides\(component\)/,
  );
  assert.match(component, /setLocalComponents\(\(current\) => current\.filter/);
  assert.doesNotMatch(component, /setPendingDeviceInstalls/);
});

test("component center derives installed ids from live inventory without mutable duplicate state", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /const installedIds = useMemo/);
  assert.match(component, /new Set\(deviceInventory\.items\.map/);
  assert.doesNotMatch(component, /setInstalledIds|useState\([^)]*installedIds/);
  assert.doesNotMatch(component, /focus-flow/);
  assert.match(component, /useState\(deviceCurrentComponent\)/);
  assert.match(component, /setActiveComponentRecord\(deviceCurrentComponent\)/);
});

test("device removal requires a live exact target and sends its board identity", () => {
  const component = readSource("ComponentCenter.jsx");
  const requestBody = component.slice(
    component.indexOf("function requestRemoveInventoryItem"),
    component.indexOf("function restoreComponentActionFocus"),
  );
  assert.match(requestBody, /deviceInventory\.freshness !== "live" \|\| !liveInventoryTarget/);
  assert.match(requestBody, /target: liveInventoryTarget/);
  assert.match(component, /boardDeviceId: target\.boardDeviceId \|\| ""/);
  assert.match(component, /sshHost: target\.sshHost \|\| ""/);
});

test("device action confirmation displays its target and manages keyboard focus", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /componentTargetLabel\(pendingComponentAction\.target\)/);
  assert.match(component, /componentActionCancelRef\.current\?\.focus\(\)/);
  assert.match(component, /event\.key === "Escape"/);
  assert.match(component, /event\.key !== "Tab"/);
  assert.match(component, /restoreComponentActionFocus\(\)/);
});

test("formal library refresh ignores stale overlapping responses", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /libraryRefreshRequestRef/);
  assert.match(component, /requestId === libraryRefreshRequestRef\.current/);
  assert.match(component, /watch\([\s\S]*componentLibraryPath/);
  assert.match(component, /setInterval\(refreshComponentLibrary, 30000\)/);
});

test("component library pins 双键接球 first and keeps other user components newest-first", () => {
  const component = readSource("ComponentCenter.jsx");
  const order = readFileSync(join(srcDir, "component-center/library-order.js"), "utf8");

  assert.match(component, /const FEATURED_BUILTIN_COMPONENT_ID = "two-key-pong"/);
  assert.match(component, /const FEATURED_BUILTIN_VERSION_HASH = "cdf23dfa806eeaad"/);
  assert.match(component, /\.\.\.featuredBuiltins,[\s\S]*\.\.\.sortComponentsByCreatedAt\(publishedItems\),[\s\S]*\.\.\.remainingBuiltins/);
  assert.match(component, /createdAtMs:\s*entry\.createdAtMs \|\| entry\.mtimeMs \|\| 0/);
  assert.match(order, /componentCreatedAtMs\(right\.component\) - componentCreatedAtMs\(left\.component\)/);
});

test("builtin catalog starts with the validated 双键接球 package", () => {
  const fixtures = readSource("fixtures.js");
  const packageRoot = join(srcDir, "../builtin-clawpkgs/two-key-pong");
  const componentManifest = JSON.parse(readFileSync(join(packageRoot, "component.json"), "utf8"));
  const widget = JSON.parse(readFileSync(join(packageRoot, "runtime/widget.json"), "utf8"));
  const buttons = JSON.parse(readFileSync(join(packageRoot, "buttons.json"), "utf8"));

  assert.match(fixtures, /components:\s*\[\s*\{\s*id: "two-key-pong"/);
  assert.equal(componentManifest.id, "two-key-pong");
  assert.equal(componentManifest.version, "1.1.1");
  assert.equal(widget.engine, "p4-bounded-runtime-v3");
  assert.deepEqual(widget.scene.grid, { width: 16, height: 16 });
  assert.deepEqual(buttons.map((binding) => binding.action), ["shift_left", "shift_right", "start"]);
});

test("a live USB handshake wins over a remembered SSH target during install", () => {
  const component = readSource("ComponentCenter.jsx");
  const installBody = component.slice(
    component.indexOf("async function performOtaInstall"),
    component.indexOf("async function installBuiltinToDevice"),
  );
  const liveStatusIndex = installBody.indexOf('invoke("usb_get_status")');
  const useSshIndex = installBody.indexOf("const useSsh =", liveStatusIndex);
  assert.ok(liveStatusIndex >= 0);
  assert.ok(useSshIndex > liveStatusIndex);
  assert.match(
    installBody,
    /const useSsh = !options\.forceUsb && !liveUsbConnected && sshHost\.length > 0/,
  );
});

test("component generation stays in the current Agent conversation through petui", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /请使用 \$petui/);
  assert.match(component, /navigator\.clipboard\.writeText\(invocationExample\)/);
  assert.match(component, /invoke\("install_widget_skill"\)/);
  assert.match(component, /正式本地组件库/);
  assert.doesNotMatch(component, /launch_agent_with_prompt/);
  assert.doesNotMatch(component, /promptDraft|createSkillTriggerPrompt|component-generation-template/);
});

test("invalid formal local components are blocked before direct device sync", () => {
  const component = readSource("ComponentCenter.jsx");
  const library = readFileSync(join(srcDir, "../src-tauri/src/component_library.rs"), "utf8");

  assert.match(component, /function localInstallBlockedReason/);
  assert.match(component, /正式本地组件暂时不能同步/);
  assert.match(component, /正式本地组件校验失败/);
  assert.match(component, /validationErrors/);
  assert.match(library, /pub valid: bool/);
  assert.match(library, /pub validation_errors: Vec<String>/);
});

test("component center matches formal library paths across Windows and POSIX separators", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /function pathContainsComponentId/);
  assert.match(component, /replaceAll\("\\\\", "\/"\)|replace\(\/\\\\\\\\\/g, "\/"\)/);
  assert.match(component, /function matchesLibraryPath/);
  assert.match(component, /matchesLibraryPath\(component, otaPendingPath\)/);
  assert.match(component, /matchesLibraryPath\(component, clawpkgPath\)/);
  assert.doesNotMatch(component, /includes\(`\/\$\{d\.id\}`\)/);
});

test("tauri no longer exposes a component-generation terminal launcher", () => {
  const tauri = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
  assert.doesNotMatch(tauri, /async fn launch_agent_with_prompt/);
  assert.doesNotMatch(tauri, /dangerously-bypass-approvals-and-sandbox/);
  assert.match(tauri, /install_widget_skill/);
});

test("petui installer uses the bundled skill and removes legacy skill directories", () => {
  const tauri = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
  assert.match(tauri, /const SKILL_NAME: &str = "petui"/);
  assert.match(tauri, /const LEGACY_SKILL_NAMES/);
  assert.match(tauri, /"petAgent-ui-generator"/);
  assert.match(tauri, /"petui-agent"/);
  assert.match(tauri, /agent: "OpenClaw"/);
  assert.match(tauri, /agent: "MiMoCode \/ Agent Skills"/);
  assert.match(tauri, /home_dir: "\.agents"/);
  assert.match(tauri, /skills_root\.join\(SKILL_NAME\)/);
});

test("tauri SSH clawpkg install checks host ssh and tar commands up front", () => {
  const tauri = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
  assert.match(tauri, /fn require_host_command/);
  assert.match(tauri, /require_host_command\("tar"/);
  assert.match(tauri, /require_host_command\("ssh"/);
  assert.match(tauri, /Command::new\(&tar_bin\)/);
  assert.match(tauri, /Command::new\(&ssh_bin\)/);
});

test("petui routes game/tool requests and publishes only validated formal components", () => {
  const skillRoot = join(srcDir, "../../skills/petui");
  const widgetSkill = readFileSync(join(skillRoot, "SKILL.md"), "utf8");
  const contract = readFileSync(join(skillRoot, "references/contract.md"), "utf8");
  const publisher = readFileSync(join(skillRoot, "scripts/publish_generated_widget.py"), "utf8");
  assert.match(widgetSkill, /name: petui/);
  assert.match(widgetSkill, /判断需求属于 `game` 还是 `tool`/);
  assert.match(widgetSkill, /不得把不支持的游戏静默替换成 Flappy Bird/);
  assert.match(widgetSkill, /validate_generated_widget\.py/);
  assert.match(widgetSkill, /publish_generated_widget\.py/);
  assert.match(widgetSkill, /优化、修复或继续迭代现有组件时[\s\S]*?保留它的 `component\.json\.id`/);
  assert.match(widgetSkill, /打开同一个组件卡片并点击“保存并同步”/);
  assert.match(contract, /`buttons\.json` 最多 8 条/);
  assert.match(contract, /button\.sw1\.short_press/);
  assert.match(contract, /knob\.rotate_cw/);
  assert.match(contract, /\.staging.*不是“草稿库”/);
  assert.match(publisher, /os\.replace\(staged_package, destination\)/);
});

test("component center preserves CreateComponentDrawer with all 3 STEP cards", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /function CreateComponentDrawer\b/);
  assert.match(component, /component-center-drawer-backdrop/);
  assert.match(component, /component-center-drawer\b/);
  assert.match(component, /Escape/);
  assert.match(component, /STEP 1.*Skill/);
  assert.match(component, /STEP 2.*生成/);
  assert.match(component, /STEP 3.*刷新组件库/);
  assert.match(component, /component-tool-card--skill/);
  assert.match(component, /component-tool-card--generate/);
  assert.match(component, /component-tool-card--refresh/);
  assert.match(component, /handleInstallSkill/);
  assert.match(component, /skillInstallResult\.installed/);
  assert.match(component, /skillInstallResult\.skipped/);
  assert.match(component, /安装 petui/);
  assert.match(component, /开打Agent（如codex）/);
  assert.match(component, /复制下方示例文案到Agent界面，与Agent对话生成你想要在设备上使用的小组件或小游戏/);
});

test("create drawer reduces step 3 to one refresh action", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /STEP 3.*刷新组件库/);
  assert.match(component, /请使用 \$petui/);
  assert.match(component, /Agent制作完成后，刷新组件库/);
  assert.match(component, /刷新后新组件会出现在组件中心/);
  assert.doesNotMatch(component, /选择并导入正式组件库|拖拽 \.clawpkg|component-clawpkg-dropzone/);
  assert.doesNotMatch(component, /importClawpkgToLibrary|handleClawpkgDrop|handleClawpkgFilePick/);
  assert.doesNotMatch(component, /invoke\("import_component_to_library"/);
});

test("component center CSS has new library rules and no old layout rules", () => {
  const styles = readSource("styles.css");
  const component = readSource("ComponentCenter.jsx");

  // New rules must exist
  assert.match(styles, /\.component-library-section\s*\{/);
  assert.match(styles, /\.component-library-grid\s*\{/);
  assert.match(styles, /\.component-center-workspace\s*\{/);
  assert.match(component, /className="component-center-workspace"/);
  assert.doesNotMatch(component, /component-center-pixel|pixel-console/);
  assert.match(styles, /\.component-center-drawer\s*\{/);
  assert.match(styles, /\.component-center-drawer-backdrop\s*\{/);
  assert.doesNotMatch(styles, /device-widget-overview|device-widget-menu|device-widget-browser/);

  // Old Plan 4 layout rules must be gone
  assert.doesNotMatch(styles, /\.component-center-grid-layout\s*\{/);
  assert.doesNotMatch(styles, /\.component-center-preview-aside\s*\{/);
  assert.doesNotMatch(styles, /\.component-center-preview-empty\s*\{/);

  // Current formal-library controls stay styled; obsolete draft hooks stay gone.
  assert.match(styles, /\.candidate-card__actions/);
  assert.match(styles, /\.candidate-card__delete/);
  assert.doesNotMatch(styles, /\.component-library-location|\.component-clawpkg-dropzone/);
  assert.doesNotMatch(styles, /\.component-drafts__refresh/);
  assert.doesNotMatch(styles, /\.component-store-card--draft/);
});

test("library grid ends with a CreateNewCard placeholder tile", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /function CreateNewCard\s*\(/);
  assert.match(component, /candidate-card--create/);
  assert.match(component, /<CreateNewCard/);
  // CreateNewCard must appear after the filtered library map inside component-library-grid
  assert.match(component, /filteredLibraryItems\.map[\s\S]*?<CreateNewCard/);
});

test("CreateNewCard calls setCreateDrawerOpen when clicked", () => {
  const component = readSource("ComponentCenter.jsx");
  assert.match(component, /CreateNewCard[\s\S]*?onClick.*setCreateDrawerOpen\(true\)/);
});

test("fixtures expose 双键接球 first, three remaining games, and three tools", () => {
  const data = readSource("fixtures.js");
  assert.match(data, /export const BUILTIN_COMPONENT_CENTER/);
  const orderedIds = [
    "two-key-pong",
    "flappy-bird",
    "block-combo",
    "snake-turn",
    "tomato-clock",
    "drink-reminder",
    "token-usage",
  ];
  let previousIndex = -1;
  for (const id of orderedIds) {
    const currentIndex = data.indexOf(`id: "${id}"`);
    assert.ok(currentIndex > previousIndex, `${id} must keep the requested default order`);
    previousIndex = currentIndex;
  }
  assert.match(data, /two-key-pong/);
  assert.match(data, /block-combo/);
  assert.match(data, /snake-turn/);
  assert.match(data, /flappy-bird/);
  assert.match(data, /token-usage/);
  assert.match(data, /tomato-clock/);
  assert.match(data, /drink-reminder/);
  assert.match(data, /像素方块/);
  assert.match(data, /像素贪吃蛇/);
  assert.match(data, /Flappy Bird/);
  assert.match(data, /双键接球/);
  assert.doesNotMatch(data, /falling-catch|接住星星|catch\.left|catch\.right|catch\.start/);
  assert.doesNotMatch(data, /ten-second-tap|5秒连点|10秒连点/);
  assert.equal((data.match(/createdAt:/g) || []).length, 7);
  assert.match(data, /gameType: "blocks"/);
  assert.match(data, /gameType: "snake"/);
  assert.match(data, /gameType: "flappy"/);
  assert.match(data, /Token 仪表盘/);
  assert.match(data, /番茄钟/);
  assert.match(data, /喝水提醒/);
  assert.doesNotMatch(data, /slack-off-countdown/);
  assert.doesNotMatch(data, /摸鱼倒计时/);
  assert.equal((data.match(/kind: "game"/g) || []).length, 4);
  assert.equal((data.match(/kind: "tool"/g) || []).length, 3);
  assert.equal((data.match(/visualStyle: "pixel"/g) || []).length, 7);
  assert.equal((data.match(/visualLayout: "tool"/g) || []).length, 3);
  assert.match(data, /button\.sw1\.short_press/);
  assert.match(data, /button\.sw2\.short_press/);
  assert.match(data, /button\.encoder\.short_press/);
  assert.match(data, /shift_left/);
  assert.match(data, /shift_right/);
  assert.match(data, /flappy\.flap/);
  assert.match(data, /tomato\.start_pause/);
  assert.match(data, /reminder\.acknowledge/);
  assert.match(data, /stats\.show_total/);
  assert.match(data, /knob\.rotate_ccw/);
  assert.match(data, /P4 通用组件运行时/);
  assert.doesNotMatch(data, /promptBuilder|componentGenerator|replacementPreview/);
});

test("Flappy Bird is shipped as a complete installable builtin template", () => {
  const packageRoot = join(srcDir, "../builtin-clawpkgs/flappy-bird");
  const manifest = JSON.parse(readFileSync(join(packageRoot, "component.json"), "utf8"));
  const negativeScreen = JSON.parse(
    readFileSync(join(packageRoot, "negative-screen.json"), "utf8"),
  );
  const buttons = JSON.parse(readFileSync(join(packageRoot, "buttons.json"), "utf8"));
  const runtime = JSON.parse(readFileSync(join(packageRoot, "runtime/widget.json"), "utf8"));
  const share = JSON.parse(readFileSync(join(packageRoot, "share.json"), "utf8"));

  assert.equal(manifest.id, "flappy-bird");
  assert.equal(manifest.name, "Flappy Bird");
  assert.equal(manifest.kind, "game");
  assert.equal(negativeScreen.dashboard.visualSprite, "flappy");
  assert.equal(runtime.game.type, "flappy");
  assert.deepEqual(runtime.game.actions, { flap: "flappy.flap" });
  assert.deepEqual(
    buttons.map((binding) => binding.action),
    ["flappy.flap", "page_main"],
  );
  assert.match(share.summary, /Flappy Bird/);
});

test("tauri formal-library delete wiring is guarded by the component library module", () => {
  const tauri = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
  const library = readFileSync(join(srcDir, "../src-tauri/src/component_library.rs"), "utf8");
  assert.match(tauri, /async fn delete_component_from_library/);
  assert.match(tauri, /delete_component_from_library,/);
  assert.match(library, /component_root\.parent\(\) != Some\(root\.as_path\(\)\)/);
  assert.match(library, /拒绝删除正式组件库之外的路径/);
  assert.match(tauri, /"屏幕点击"\s*=>\s*\("屏幕区域",\s*"screen\.region\.tap"\)/);
});

test("tauri formal-library listing exposes preview and source metadata", () => {
  const library = readFileSync(join(srcDir, "../src-tauri/src/component_library.rs"), "utf8");
  assert.match(library, /pub struct ComponentLibraryEntry/);
  assert.match(library, /pub description:\s*String/);
  assert.match(library, /pub buttons:\s*Vec<serde_json::Value>/);
  assert.match(library, /pub version_hash:\s*String/);
  assert.match(library, /pub struct ComponentLibrarySnapshot/);
  assert.match(library, /pub library_path:\s*String/);
});

test("component center omits the manual formal-library import flow", () => {
  const component = readSource("ComponentCenter.jsx");
  const tauri = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
  assert.doesNotMatch(component, /function importClawpkgToLibrary/);
  assert.doesNotMatch(component, /invoke\("import_component_to_library"/);
  assert.doesNotMatch(component, /openDialog|handleClawpkgDrop|handleClawpkgFilePick/);
  assert.doesNotMatch(tauri, /import_component_to_library/);
  assert.match(component, /async function installClawpkgFromPath/);
});

test("component install keeps its button map package-owned without replacing device navigation", () => {
  const component = readSource("ComponentCenter.jsx");
  const tauri = readFileSync(join(srcDir, "../src-tauri/src/lib.rs"), "utf8");
  const firmware = readFileSync(join(srcDir, "../../esp-p4-runtime/main/pet_p4_input.c"), "utf8");
  const miniapp = readFileSync(join(srcDir, "../../esp-p4-runtime/main/pet_p4_miniapp.c"), "utf8");
  assert.doesNotMatch(component, /buildComponentButtonConfigBindings/);
  assert.doesNotMatch(component, /invoke\("button_config_signal"/);
  assert.doesNotMatch(component, /applyComponentButtonConfig/);
  assert.doesNotMatch(component, /DEVICE_BUTTON_CONFIG_STORAGE_KEY/);
  assert.match(component, /bindingOverrides: buildBindingOverridesForInstall\(component\)/);
  assert.match(firmware, /dispatch_component_binding_event/);
  assert.match(firmware, /strcmp\(state->screen_page, "app"\) != 0/);
  assert.match(firmware, /pet_p4_miniapp_resolve_input/);
  assert.match(firmware, /strcmp\(binding->action, "miniapp_action"\)/);
  assert.match(miniapp, /bool pet_p4_miniapp_resolve_input/);
  const removeBody = tauri.slice(
    tauri.indexOf("async fn remove_widget_from_device"),
    tauri.indexOf("fn canonical_binding_for_control"),
  );
  assert.doesNotMatch(removeBody, /reset_input_config/);
});
