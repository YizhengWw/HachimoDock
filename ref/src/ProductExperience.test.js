/**
 * [Input] Product experience bug report and core Pet Manager source files.
 * [Output] Static Node regression coverage for top-level shell routing including a mounted-while-bound dashboard service lifecycle, centralized API configuration, Tauri-first setup routing, component center routing, flattened
 *          desktop HTTP bridge calls, fixed-height desktop sidebar, unified pet album naming, USB-only single desktop-pet assignment,
 *          dashboard guide entry, faster previews, shared-provider wizard help affordances, target-specific install-relative Tauri resources,
 *          stable and config-versioned local macOS Accessibility packaging, relocatable bundled runtime enforcement, source-matched P4 ready-cache reuse, binding-scoped appearance detail, Tauri local-media playback, and the narrowly scoped IMG.LY model-download CSP needed by avatar background removal.
 * [Pos] test node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, posix, win32, join, resolve } from "node:path";

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

test("desktop CSP permits only the official background-removal model CDN", () => {
  const config = JSON.parse(readSource("src-tauri/tauri.conf.json"));
  const csp = config.app.security.csp;

  assert.match(csp, /connect-src[^;]*https:\/\/staticimgly\.com/);
  assert.doesNotMatch(csp, /connect-src[^;]*https:\s/);
  assert.match(csp, /script-src[^;]*blob:/);
});

test("desktop CSP permits Tauri local appearance images and videos", () => {
  const config = JSON.parse(readSource("src-tauri/tauri.conf.json"));
  const csp = config.app.security.csp;

  assert.match(csp, /img-src[^;]*http:\/\/asset\.localhost/);
  assert.match(csp, /media-src[^;]*http:\/\/asset\.localhost/);
});

test("release CSP and Cargo features exclude development-only privileges", () => {
  const config = JSON.parse(readSource("src-tauri/tauri.conf.json"));
  const devConfig = JSON.parse(readSource("src-tauri/tauri.dev.conf.json"));
  const cargo = readSource("src-tauri/Cargo.toml");
  const packageJson = JSON.parse(readSource("package.json"));
  const viteConfig = readSource("vite.config.js");
  const cspBuildGuard = readSource("../scripts/check-release-csp.mjs");
  const bridgePackage = JSON.parse(readSource("src-tauri/bridge/package.json"));
  const bridgeLock = readSource("src-tauri/bridge/package-lock.json");
  const csp = config.app.security.csp;
  const devCsp = devConfig.app.security.csp;

  assert.equal(csp.includes("'unsafe-eval'"), false);
  assert.doesNotMatch(csp, /script-src[^;]*'unsafe-inline'/);
  assert.match(csp, /script-src[^;]*'wasm-unsafe-eval'/);
  assert.doesNotMatch(csp, /127\.0\.0\.1:\*|localhost:\*|ws:\/\//);
  assert.match(csp, /connect-src[^;]*http:\/\/127\.0\.0\.1:8181(?:\s|;)/);
  assert.doesNotMatch(csp, /(?:img-src|media-src)[^;]*127\.0\.0\.1/);
  assert.match(devCsp, /connect-src[^;]*ws:\/\/localhost:4173/);
  assert.match(cargo, /\[features\][\s\S]*?devtools\s*=\s*\["tauri\/devtools"\]/);
  assert.doesNotMatch(cargo, /tauri\s*=\s*\{[^\n]*features\s*=\s*\[[^\]]*"devtools"/);
  assert.match(packageJson.scripts.dev, /tauri dev --features devtools --config src-tauri\/tauri\.dev\.conf\.json/);
  assert.doesNotMatch(packageJson.scripts["build:win"], /devtools/);
  assert.doesNotMatch(packageJson.scripts["build:mac"], /devtools/);
  assert.match(viteConfig, /cspSafeBackgroundRemovalPlugin\(\)/);
  assert.match(packageJson.scripts["build:web"], /check:release-csp/);
  assert.match(cspBuildGuard, /Function constructor/);
  assert.match(cspBuildGuard, /eval/);
  assert.equal(bridgePackage.main, undefined);
  assert.equal(bridgePackage.build, undefined);
  assert.equal(bridgePackage.scripts.start, undefined);
  assert.equal(bridgePackage.devDependencies, undefined);
  assert.equal(bridgePackage.dependencies, undefined);
  assert.doesNotMatch(bridgeLock, /node_modules\/electron(?:-builder|-updater)?"/);
});

test("Tauri release resources are target-specific, install-relative, and reproducible", () => {
  const config = JSON.parse(readSource("src-tauri/tauri.conf.json"));
  const macosConfig = JSON.parse(readSource("src-tauri/tauri.macos.conf.json"));
  const windowsConfig = JSON.parse(readSource("src-tauri/tauri.windows.conf.json"));
  const packageJson = JSON.parse(readSource("package.json"));
  const packageLock = JSON.parse(readSource("package-lock.json"));
  const cargoManifest = readSource("src-tauri/Cargo.toml");
  const prepareScript = readSource("../scripts/prepare-desktop-resources.mjs");
  const p4ReadyScript = readSource("../scripts/prepare-p4-ready-assets.mjs");
  const localMacSigner = readSource("../scripts/sign-macos-local-app.mjs");
  const localMacDmgBundler = readSource("../scripts/bundle-macos-local-dmg.mjs");
  const crossInstaller = readSource("../scripts/build-windows-nsis-cross.mjs");
  const tauri = readSource("src-tauri/src/lib.rs");
  const ffmpeg = readSource("src-tauri/src/codex_import.rs");
  const resources = config.bundle.resources;

  assert.equal(config.version, packageJson.version);
  assert.equal(packageLock.version, packageJson.version);
  assert.equal(packageLock.packages[""].version, packageJson.version);
  assert.match(cargoManifest, new RegExp(`^version = "${packageJson.version.replaceAll(".", "\\.")}"$`, "m"));

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
  assertResource(
    resources,
    "bridge/packages/clawd-backend-service/node_modules",
    "bridge/packages/clawd-backend-service/node_modules",
  );
  assert.equal(resources["bridge/runtime/node"], undefined);
  assert.equal(resources["bridge/runtime/node.exe"], undefined);
  assertResource(
    macosConfig.bundle.resources,
    "generated-runtime/node",
    "bridge/runtime/node",
  );
  assertResource(
    macosConfig.bundle.resources,
    "generated-runtime/ffmpeg",
    "tools/ffmpeg",
  );
  assertResource(
    macosConfig.bundle.resources,
    "generated-runtime/ffmpeg.LICENSE",
    "tools/ffmpeg.LICENSE",
  );
  assertResource(
    macosConfig.bundle.resources,
    "generated-runtime/ffmpeg.README",
    "tools/ffmpeg.README",
  );
  assertResource(
    macosConfig.bundle.resources,
    "generated-runtime/ffmpeg.SOURCE.txt",
    "tools/ffmpeg.SOURCE.txt",
  );
  assert.equal(macosConfig.bundle.macOS.signingIdentity, "-");
  assert.match(packageJson.scripts["build:mac:local"], /sign:mac:local/);
  assert.match(packageJson.scripts["build:mac:local"], /tauri build --bundles app,dmg --ci/);
  assert.match(packageJson.scripts["build:mac:local"], /bundle:mac:local/);
  assert.match(packageJson.scripts["build:mac:local"], /verify:mac:local/);
  assert.match(packageJson.scripts["sign:mac:local"], /sign-macos-local-app\.mjs/);
  assert.match(packageJson.scripts["bundle:mac:local"], /bundle-macos-local-dmg\.mjs/);
  assert.match(packageJson.scripts["verify:mac:local"], /--verify-only/);
  assert.match(localMacSigner, /designated => identifier "com\.petmanager\.desktop"/);
  assert.match(localMacSigner, /--verify/);
  assert.match(localMacSigner, /stable local designated requirement is missing/);
  assert.match(localMacDmgBundler, /verifyStableApp\(appPath\)/);
  assert.match(localMacDmgBundler, /readFileSync\(tauriConfigPath, "utf8"\)/);
  assert.match(localMacDmgBundler, /\$\{productName\}_\$\{appVersion\}_\$\{dmgArchitecture\}\.dmg/);
  assert.match(localMacDmgBundler, /verifyStableApp\(join\(mountRoot, appBundleName\)\)/);
  assert.doesNotMatch(localMacDmgBundler, /Pet Manager_0\.1\.0/);
  assert.match(localMacDmgBundler, /hdiutil/);
  assert.match(localMacDmgBundler, /--skip-jenkins/);
  assertResource(
    windowsConfig.bundle.resources,
    "generated-runtime/node.exe",
    "bridge/runtime/node.exe",
  );
  assertResource(
    windowsConfig.bundle.resources,
    "generated-runtime/ffmpeg.exe",
    "tools/ffmpeg.exe",
  );
  assertResource(
    windowsConfig.bundle.resources,
    "generated-runtime/ffmpeg.LICENSE",
    "tools/ffmpeg.LICENSE",
  );
  assertResource(
    windowsConfig.bundle.resources,
    "generated-runtime/ffmpeg.README",
    "tools/ffmpeg.README",
  );
  assertResource(
    windowsConfig.bundle.resources,
    "generated-runtime/ffmpeg.SOURCE.txt",
    "tools/ffmpeg.SOURCE.txt",
  );
  assertResource(resources, "../../skills/petui", "skills/petui");

  for (const platformResources of [
    resources,
    macosConfig.bundle.resources,
    windowsConfig.bundle.resources,
  ]) {
    for (const [source, target] of Object.entries(platformResources)) {
      assert.equal(
        posix.isAbsolute(source) || win32.isAbsolute(source),
        false,
        `resource source must be relative: ${source}`,
      );
      assert.equal(
        posix.isAbsolute(target) || win32.isAbsolute(target),
        false,
        `resource target must be relative: ${target}`,
      );
    }
  }

  assert.ok(existsSync(join(refRoot, "public/terrier-clips")), "public terrier clips should feed the Vite dist resource");
  assertTauriResourceSourceExists("../builtin-clawpkgs");
  assertTauriResourceSourceExists("bridge/packages/clawd-backend-service/src");
  assertTauriResourceSourceExists("bridge/packages/agent-session-bus/src");
  assertTauriResourceSourceExists("bridge/agents");
  assertTauriResourceSourceExists("bridge/hooks");
  assertTauriResourceSourceExists("../../skills/petui");
  assert.ok(existsSync(join(refRoot, "src-tauri/bridge/packages/clawd-backend-service/package-lock.json")));

  assert.match(tauri, /resource_dir\.join\("terrier-clips"\)/);
  assert.match(tauri, /resource_dir\.join\("builtin-clawpkgs"\)\.join\(id\)/);
  assert.match(tauri, /resource_dir\.join\(BRIDGE_RESOURCE_ROOT\)/);
  assert.match(tauri, /resource_dir\.join\("bridge\/runtime"\)\.join\(node_name\)/);
  assert.match(tauri, /res_dir\.join\("skills"\)\.join\(SKILL_NAME\)/);
  assert.match(packageJson.scripts["build:win"], /prepare-desktop-resources\.mjs --target windows/);
  assert.match(packageJson.scripts["build:win"], /x86_64-pc-windows-msvc/);
  assert.match(packageJson.scripts["build:win"], /--bundles nsis,msi/);
  assert.match(prepareScript, /PET_MANAGER_NODE_BIN/);
  assert.match(prepareScript, /PET_MANAGER_FFMPEG_BIN/);
  assert.match(prepareScript, /FFmpeg 8\.1\.2 macOS arm64/);
  assert.match(prepareScript, /ffmpegStaticRelease = "b6\.1\.1"/);
  assert.match(prepareScript, /archiveSha256/);
  assert.match(prepareScript, /binarySha256/);
  assert.match(prepareScript, /ffmpeg\.LICENSE/);
  assert.match(prepareScript, /ffmpeg\.SOURCE\.txt/);
  assert.match(prepareScript, /--enable-nonfree/);
  assert.match(prepareScript, /依法不可随安装包分发/);
  assert.match(prepareScript, /Git LFS 指针/);
  assert.match(prepareScript, /assertRuntimeMatchesTarget/);
  assert.match(prepareScript, /\/usr\/bin\/otool/);
  assert.match(prepareScript, /依赖未随应用打包的动态库/);
  assert.match(prepareScript, /reusing validated staged Node runtime/);
  assert.match(prepareScript, /existsSync\(targetRuntime\)/);
  assert.match(p4ReadyScript, /copyValidCachedAudio/);
  assert.match(p4ReadyScript, /cachedAudioHashes\.get\(family\) === audioHash/);
  assert.match(p4ReadyScript, /Git LFS 指针/);
  assert.match(p4ReadyScript, /git lfs pull/);
  assert.match(prepareScript, /npmArgs = \["ci", "--omit=dev"/);
  assert.match(ffmpeg, /bundled_ffmpeg_candidates/);
  assert.match(ffmpeg, /join\("Resources"\)\.join\("tools"\)\.join\("ffmpeg"\)/);
  assert.match(ffmpeg, /join\("tools"\)\.join\("ffmpeg\.exe"\)/);
  assert.match(crossInstaller, /join\(stageRoot, "tools", "ffmpeg\.exe"\)/);
  assert.match(crossInstaller, /join\(stageRoot, "tools", "ffmpeg\.LICENSE"\)/);

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
  assert.doesNotMatch(tauri, /dev_bundled/);
});

test("desktop packages statically vendor libusb instead of linking a build-machine path", () => {
  const cargoManifest = readSource("src-tauri/Cargo.toml");

  assert.match(
    cargoManifest,
    /rusb\s*=\s*\{\s*version\s*=\s*"0\.9",\s*features\s*=\s*\["vendored"\]\s*\}/,
  );
});

test("coding-agent and media-tool detection uses user/environment-derived paths", () => {
  const tauri = readSource("src-tauri/src/lib.rs");
  const ffmpeg = readSource("src-tauri/src/codex_import.rs");
  const composer = readSource("src-tauri/src/codex_composer_macos.rs");
  const codex = readSource("src-tauri/bridge/packages/agent-session-bus/src/adapters/codex.js");
  const claude = readSource("src-tauri/bridge/packages/agent-session-bus/src/adapters/claude-code.js");
  const mimocode = readSource("src-tauri/bridge/packages/agent-session-bus/src/adapters/mimocode.js");
  const openclaw = readSource("src-tauri/bridge/packages/agent-session-bus/src/util/openclaw-paths.js");

  for (const [label, source] of Object.entries({
    tauri,
    ffmpeg,
    composer,
    codex,
    claude,
    mimocode,
    openclaw,
  })) {
    assert.doesNotMatch(source, /\/opt\/homebrew|\/usr\/local|\/usr\/bin|C:\\\\ffmpeg/, label);
  }
  assert.doesNotMatch(tauri, /PathBuf::from\("\/Applications"\)/);
  assert.doesNotMatch(codex, /"\/Applications\//);
  assert.match(tauri, /get_full_shell_path\(\)/);
  assert.match(tauri, /get_full_path_from_registry\(\)/);
  assert.match(tauri, /LOCALAPPDATA/);
  assert.match(tauri, /macos_app_bundle_candidates/);
  assert.match(tauri, /kMDItemCFBundleIdentifier/);
  assert.match(tauri, /merged_host_path/);
});

test("app shell routes device, pet album, component center, API settings, detail, wizard, and generation toast from the sidebar", () => {
  const app = readSource("src/App.jsx");

  assert.match(app, /const \[view, setView\] = useState\("loading"\)/);
  assert.match(app, /const \[detailAppearanceId, setDetailAppearanceId\] = useState\(""\)/);
  assert.match(app, /DEV_DIRECT_DASHBOARD_BINDING/);
  assert.match(app, /function hasTauriRuntime\(\)/);
  assert.match(app, /return typeof window !== "undefined" && Boolean\(window\.__TAURI_INTERNALS__\);/);
  assert.match(app, /import\.meta\.env\.DEV && !hasTauriRuntime\(\) \? DEV_DIRECT_DASHBOARD_BINDING : null/);
  assert.match(app, /enterBestAvailableDeviceSurface/);
  assert.match(app, /const galleryViews = new Set\(\["gallery", "wizard", "detail"\]\);/);
  assert.match(app, /const activeTab = view === "api"/);
  assert.match(app, /const handleOpenGallery = useCallback/);
  assert.match(app, /const handleOpenComponents = useCallback/);
  assert.match(app, /const handleOpenApiSettings = useCallback/);
  assert.match(app, /const handleEnterWizard = useCallback\(\(\) => setView\("wizard"\)/);
  assert.match(app, /const handleOpenDetail = useCallback/);
  assert.match(app, /const handleDetailBack = useCallback/);
  assert.match(app, /subscribeGenerationTask/);
  assert.match(app, /s\.status === "completed"[\s\S]*refresh\(\)\.catch/);
  assert.match(app, /acknowledgeGenerationTask/);
  assert.match(app, /<AppearanceGallery/);
  assert.match(app, /<CustomAvatarWizard/);
  assert.match(app, /<AppearanceDetail/);
  assert.match(app, /<ComponentCenter \/>/);
  assert.match(app, /<ApiSettings/);
  assert.doesNotMatch(app, /CommunityImportHelp/);
});

test("bound dashboard stays mounted while other pages are visible", () => {
  const app = readSource("src/App.jsx");

  assert.match(app, /\{binding && \(\s*<div hidden=\{!isDashboard\}>/);
  assert.doesNotMatch(app, /\{isDashboard && binding && \(/);
});

test("appearance detail receives the current board binding for exact native USB sync", () => {
  const source = readSource("src/App.jsx");

  assert.match(source, /<AppearanceDetail[\s\S]*boardDeviceId=\{binding\?\.boardDeviceId \|\| ""\}/);
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
  assert.match(channelMatrix, /agentOptions\.filter\(\(agent\) => agent\?\.id\)/);
  assert.match(channelMatrix, /BUILTIN_TERRIER_APPEARANCE_ID/);
  assert.match(channelMatrix, /saveAgentAppearance\(agentId, appearance\.id\)/);
  assert.match(channelMatrix, /setPendingFollow\(\{ agentId, appearance \}\)/);
  assert.match(channelMatrix, /applyDesktopPet\(agentId, appearance/);
  assert.match(channelMatrix, /formosa-picker__grid/);
  assert.match(channelMatrix, /usb\?\.connected/);
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
  assert.match(preview, /IntersectionObserver/);
  assert.match(preview, /visibilitychange/);
  assert.match(preview, /preload=\{shouldPlay \? "auto" : "metadata"\}/);
  assert.match(preview, /loading="lazy"/);
  assert.match(preview, /decoding="async"/);
  assert.match(gallery, /codex pet/);
});

test("generation setup clearly supports GIF first-frame input and field help affordances", () => {
  const wizard = readSource("src/CustomAvatarWizard.jsx");
  const providerConfig = readSource("src/lib/avatar-pipeline/provider-config.js");

  assert.match(wizard, /image\/gif/);
  assert.match(wizard, /GIF 会取首帧作为参考图/);
  assert.match(wizard, /FieldWithHelp/);
  assert.match(wizard, /generation-api-status/);
  assert.match(wizard, /打开 API 配置/);
  assert.doesNotMatch(wizard, /type="password"/);
  assert.match(wizard, /label="Base URL"/);
  assert.match(wizard, /label="视频生成模型"/);
  assert.match(providerConfig, /VOLCENGINE_THINKING_MODEL = DEFAULT_THINKING_MODEL/);
  assert.match(wizard, /loadProviderConfig/);
  assert.match(wizard, /saveProviderConfig/);
  assert.match(wizard, /thinkingModel: isVolcengine \? VOLCENGINE_THINKING_MODEL : thinkingModel\.trim\(\) \|\| trimmedModel/);
  assert.match(wizard, /请先在 API 配置中保存火山引擎 API Key/);
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
