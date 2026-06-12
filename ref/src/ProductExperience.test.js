/**
 * [Input] Product experience bug report and core HachimoDock source files.
 * [Output] Static Node regression coverage for top-level shell routing, Tauri-first setup routing, component center routing, flattened
 *          desktop HTTP bridge calls, fixed-height desktop sidebar, unified pet album naming, modal-based single desktop-pet assignment,
 *          dashboard guide entry, faster previews, wizard help affordances, and packaged Tauri runtime resources.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

const srcDir = dirname(fileURLToPath(import.meta.url));
const refRoot = resolve(srcDir, "..");

function readSource(relativePath) {
  return readFileSync(join(refRoot, relativePath), "utf8");
}

function assertResource(resources, source, target) {
  assert.equal(resources[source], target, `${source} should bundle as ${target}`);
}

function assertTauriResourceSourceExists(source) {
  assert.ok(
    existsSync(resolve(refRoot, "src-tauri", source)),
    `${source} should exist before Tauri bundles it`,
  );
}

function assertDebugOnlyFallback(source, snippet, label) {
  const index = source.indexOf(snippet);
  assert.notEqual(index, -1, `${label} fallback should still be present for dev builds`);

  const functionStart = source.lastIndexOf("\nfn ", index);
  const debugGuard = source.lastIndexOf("#[cfg(debug_assertions)]", index);
  const releaseGuard = source.lastIndexOf("#[cfg(not(debug_assertions))]", index);

  assert.ok(
    debugGuard > functionStart,
    `${label} fallback must be guarded by #[cfg(debug_assertions)]`,
  );
  assert.ok(
    releaseGuard < functionStart || releaseGuard < debugGuard,
    `${label} fallback must not live under #[cfg(not(debug_assertions))]`,
  );
}

test("desktop window opens at the configured minimum size and remains centered", () => {
  const config = JSON.parse(readSource("src-tauri/tauri.conf.json"));
  const [mainWindow] = config.app.windows;

  assert.equal(mainWindow.width, mainWindow.minWidth);
  assert.equal(mainWindow.height, mainWindow.minHeight);
  assert.equal(mainWindow.center, true);
});

test("Tauri release runtime resources are packaged and development path fallbacks stay debug-only", () => {
  const config = JSON.parse(readSource("src-tauri/tauri.conf.json"));
  const tauri = readSource("src-tauri/src/lib.rs");
  const resources = config.bundle.resources;

  assertResource(resources, "../dist/terrier-clips", "terrier-clips");
  assertResource(resources, "../builtin-clawpkgs", "builtin-clawpkgs");
  assertResource(resources, "bridge/package.json", "bridge/package.json");
  assertResource(
    resources,
    "bridge/packages/clawd-backend-service/src",
    "bridge/packages/clawd-backend-service/src",
  );
  assertResource(
    resources,
    "bridge/packages/agent-session-bus/src",
    "bridge/packages/agent-session-bus/src",
  );
  assertResource(resources, "bridge/agents", "bridge/agents");
  assertResource(resources, "bridge/hooks", "bridge/hooks");
  assertResource(resources, "bridge/runtime/node", "bridge/runtime/node");
  assertResource(resources, "bridge/runtime/node.exe", "bridge/runtime/node.exe");
  assertResource(resources, "../../skills/petAgent-ui-generator", "skills/petAgent-ui-generator");

  assert.ok(existsSync(join(refRoot, "public/terrier-clips")), "public terrier clips should feed the Vite dist resource");
  assertTauriResourceSourceExists("../builtin-clawpkgs");
  assertTauriResourceSourceExists("bridge/packages/clawd-backend-service/src");
  assertTauriResourceSourceExists("bridge/packages/agent-session-bus/src");
  assertTauriResourceSourceExists("bridge/agents");
  assertTauriResourceSourceExists("bridge/hooks");
  assertTauriResourceSourceExists("bridge/runtime/node");
  assertTauriResourceSourceExists("bridge/runtime/node.exe");
  assertTauriResourceSourceExists("../../skills/petAgent-ui-generator");

  assert.match(tauri, /resource_dir\.join\("terrier-clips"\)/);
  assert.match(tauri, /resource_dir\.join\("builtin-clawpkgs"\)\.join\(id\)/);
  assert.match(tauri, /resource_dir\.join\(BRIDGE_RESOURCE_ROOT\)/);
  assert.match(tauri, /resource_dir\.join\("bridge\/runtime"\)\.join\(node_name\)/);
  assert.match(tauri, /res_dir\.join\("skills"\)\.join\(SKILL_NAME\)/);

  assertDebugOnlyFallback(
    tauri,
    'let manifest_dir = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));',
    "widget skill source-tree",
  );
  assertDebugOnlyFallback(
    tauri,
    'let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));',
    "built-in clawpkg source-tree",
  );
  assertDebugOnlyFallback(
    tauri,
    'home.join(".openclaw").join("builtin-clawpkgs").join(id)',
    "legacy built-in clawpkg user-dir",
  );
  assert.doesNotMatch(tauri, /#\[cfg\(not\(debug_assertions\)\]\s*candidates\.push\(dev_bridge_root\)/);
  assertDebugOnlyFallback(
    tauri,
    '"../../../openclaw-pet/voice-service-node"',
    "voice-service source-tree",
  );
  assertDebugOnlyFallback(
    tauri,
    'let dev_bundled = PathBuf::from(env!("CARGO_MANIFEST_DIR"))',
    "Node runtime source-tree",
  );
});

test("app shell routes device, pet album, component center, detail, wizard, and generation toast from the sidebar", () => {
  const app = readSource("src/App.jsx");

  assert.match(app, /const \[view, setView\] = useState\("loading"\)/);
  assert.match(app, /const \[detailAppearanceId, setDetailAppearanceId\] = useState\(""\)/);
  assert.match(app, /DEV_DIRECT_DASHBOARD_BINDING/);
  assert.match(app, /function hasTauriRuntime\(\)/);
  assert.match(app, /return typeof window !== "undefined" && Boolean\(window\.__TAURI_INTERNALS__\);/);
  assert.match(app, /import\.meta\.env\.DEV && !hasTauriRuntime\(\) \? DEV_DIRECT_DASHBOARD_BINDING : null/);
  assert.match(app, /enterBestAvailableDeviceSurface/);
  assert.match(app, /const galleryViews = new Set\(\["gallery", "wizard", "detail"\]\);/);
  assert.match(app, /const activeTab = view === "components" \? "components" : galleryViews\.has\(view\) \? "gallery" : "device";/);
  assert.match(app, /const handleOpenGallery = useCallback/);
  assert.match(app, /const handleOpenComponents = useCallback/);
  assert.match(app, /const handleEnterWizard = useCallback\(\(\) => setView\("wizard"\)/);
  assert.match(app, /const handleOpenDetail = useCallback/);
  assert.match(app, /const handleDetailBack = useCallback/);
  assert.match(app, /subscribeGenerationTask/);
  assert.match(app, /acknowledgeGenerationTask/);
  assert.match(app, /<AppearanceGallery/);
  assert.match(app, /<CustomAvatarWizard/);
  assert.match(app, /<AppearanceDetail/);
  assert.match(app, /<ComponentCenter \/>/);
  assert.doesNotMatch(app, /CommunityImportHelp/);
});

test("desktop sidebar stays viewport-bound instead of stretching with page height", () => {
  const styles = readSource("src/styles.css");

  assert.match(styles, /\.app-sidebar\s*{[\s\S]*position:\s*sticky/);
  assert.match(styles, /\.app-sidebar\s*{[\s\S]*top:\s*0/);
  assert.match(styles, /\.app-sidebar\s*{[\s\S]*height:\s*100vh/);
  assert.match(styles, /\.app-sidebar\s*{[\s\S]*overflow-y:\s*auto/);
  assert.match(styles, /@media\s*\(max-width:\s*1080px\)[\s\S]*\.app-sidebar\s*{[\s\S]*position:\s*static/);
  assert.match(styles, /@media\s*\(max-width:\s*1080px\)[\s\S]*\.app-sidebar\s*{[\s\S]*height:\s*auto/);
});

test("device dashboard remains the management surface with one desktop-pet assignment", () => {
  const dashboard = readSource("src/DeviceDashboard.jsx");
  // Channel/appearance logic now lives in ChannelMatrixCard.
  const channelMatrix = readSource("src/dashboard/ChannelMatrixCard.jsx");

  assert.match(channelMatrix, /resolveDashboardPreviewMedia/);
  assert.match(dashboard, /title="Agent与形象"/);
  assert.match(channelMatrix, /agentOptions\.filter\(\(agent\) => agent\.detected\)/);
  assert.match(channelMatrix, /BUILTIN_TERRIER_APPEARANCE_ID/);
  assert.match(channelMatrix, /saveAgentAppearance\(agentId, appearance\.id\)/);
  assert.match(channelMatrix, /setPendingFollow\(\{ agentId, appearance \}\)/);
  assert.match(channelMatrix, /applyDesktopPet\(agentId, appearance/);
  assert.match(channelMatrix, /formosa-picker__grid/);
  assert.match(channelMatrix, /deviceConnected/);
  assert.doesNotMatch(channelMatrix, /ChannelSwitchConfirmModal/);
  assert.doesNotMatch(channelMatrix, /desktop-pet-channel-expanded__apply/);
  // Guide modal stays in the orchestrator.
  assert.match(dashboard, /DeviceGuideModal/);
  assert.match(dashboard, /ChannelMatrixCard/);
});

test("appearance listing and previews keep source fallbacks and dashboard-specific media resolution", () => {
  const previewHelper = readSource("src/lib/appearance-preview.js");
  const gallery = readSource("src/AppearanceGallery.jsx");
  const preview = readSource("src/AppearancePreview.jsx");

  assert.match(previewHelper, /export function resolveDashboardPreviewMedia/);
  assert.match(previewHelper, /mediaFromSourceImage\(record\)/);
  assert.match(preview, /preload=\{playing \? "auto" : "metadata"\}/);
  assert.match(preview, /loading="lazy"/);
  assert.match(preview, /decoding="async"/);
  assert.match(gallery, /codex pet/);
});

test("generation setup clearly supports GIF first-frame input and field help affordances", () => {
  const wizard = readSource("src/CustomAvatarWizard.jsx");

  assert.match(wizard, /image\/gif/);
  assert.match(wizard, /GIF 会取首帧作为参考图/);
  assert.match(wizard, /FieldWithHelp/);
  assert.match(wizard, /label="API Key"/);
  assert.match(wizard, /label="Base URL"/);
  assert.match(wizard, /label="视频生成模型"/);
  assert.match(wizard, /const VOLCENGINE_THINKING_MODEL = DEFAULT_THINKING_MODEL/);
  assert.match(wizard, /thinkingModel: isVolcengine \? VOLCENGINE_THINKING_MODEL : thinkingModel\.trim\(\) \|\| trimmedModel/);
  assert.match(wizard, /请先填写 API Key 和视频生成模型/);
  assert.match(wizard, /火山 Ark 地址已固定/);
  assert.doesNotMatch(wizard, /Thinking 模型 endpoint/);
  assert.doesNotMatch(wizard, /providerId === "volcengine" && !thinkingModel\.trim\(\)/);
  assert.doesNotMatch(wizard, /推理接入点 \/ Endpoint/);
});

test("generation defaults still favor faster Ark-safe low-resolution video output", () => {
  const wizard = readSource("src/CustomAvatarWizard.jsx");
  const run = readSource("src/lib/avatar-pipeline/run.js");
  const defaults = readSource("src/lib/avatar-pipeline/pipeline-defaults.js");

  assert.match(wizard, /fastGeneration/);
  assert.match(wizard, /FAST_VIDEO_GENERATION_PROFILE\.imageMaxDimension/);
  assert.match(defaults, /PIPELINE_MAX_IMAGE_DIMENSION = 400/);
  assert.match(defaults, /400 keeps 4:3 height at Ark's 300px minimum/);
  assert.match(defaults, /FAST_VIDEO_GENERATION_PROFILE/);
  assert.match(run, /resolveGenerationSpeedConfig/);
  assert.match(run, /resolveThinkingModelName/);
});

test("desktop avatar generation requests use flattened Rust bridge arguments before any fallback", () => {
  const http = readSource("src/lib/avatar-pipeline/http.js");
  const tauri = readSource("src-tauri/src/lib.rs");

  assert.match(http, /invoke\("http_request_text"/);
  assert.match(http, /headersJson: JSON\.stringify\(normalizeHeaders\(init\.headers\)\)/);
  assert.match(http, /timeoutMs: typeof init\?\.timeoutMs === "number" \? init\.timeoutMs : undefined/);
  assert.doesNotMatch(http, /input:\s*\{/);
  assert.match(http, /http_request_text unavailable, falling back/);

  assert.match(tauri, /async fn http_request_text\(/);
  assert.match(tauri, /url: String,/);
  assert.match(tauri, /method: Option<String>,/);
  assert.match(tauri, /headers_json: Option<String>,/);
  assert.match(tauri, /body: Option<String>,/);
  assert.match(tauri, /timeout_ms: Option<u64>,/);
  assert.match(tauri, /serde_json::from_str\(&raw\)/);
  assert.match(tauri, /return Err\(/);
});
