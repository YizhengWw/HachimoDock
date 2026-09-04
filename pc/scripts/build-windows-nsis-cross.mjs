/**
 * [Input] A cargo-xwin-built Pet Manager x64 executable and prepared firmware, Node, audited LGPL FFmpeg, notice/source, bridge, and WebView resource sources.
 * [Output] A current-user NSIS installer with exact validated Windows P4 firmware, install-relative Node and LGPL FFmpeg resources, complete corresponding source/notices, and embedded WebView2 bootstrapper.
 * [Pos] Reproducible macOS-to-Windows installer bundler used after `build:win:portable`.
 * [Sync] If this file changes, update `scripts/.folder.md` and `docs/desktop-packaging.md`.
 */

import {
  copyFileSync,
  cpSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDir, "../..");
const refRoot = join(repositoryRoot, "pc");
const tauriRoot = join(refRoot, "src-tauri");
const releaseRoot = join(
  tauriRoot,
  "target",
  "x86_64-pc-windows-msvc",
  "release",
);
const bundleRoot = join(releaseRoot, "bundle", "nsis");
const stageRoot = join(releaseRoot, "bundle", "nsis-cross-stage");
const stopOwnedRuntimesScript = join(
  tauriRoot,
  "installer",
  "windows",
  "stop-owned-runtimes.ps1",
);
const version = JSON.parse(readFileSync(join(refRoot, "package.json"), "utf8")).version;
const appExeSource = resolve(
  process.env.PET_MANAGER_WINDOWS_EXE
    || join(releaseRoot, "pet-manager-tauri.exe"),
);
const appExeName = "Pet Manager.exe";
const installerName = `Pet_Manager_${version}_x64-setup.exe`;
const installerPath = join(bundleRoot, installerName);
const webviewBootstrapperUrl =
  "https://go.microsoft.com/fwlink/p/?LinkId=2124703";

function assertPeExecutable(path, label) {
  if (!existsSync(path)) throw new Error(`${label} 不存在：${path}`);
  const bytes = readFileSync(path);
  if (bytes.length < 2 || bytes[0] !== 0x4d || bytes[1] !== 0x5a) {
    throw new Error(`${label} 不是 Windows PE 可执行文件：${path}`);
  }
}

function assertExactFileCopy(source, destination, label) {
  if (!existsSync(source)) throw new Error(`${label} 源文件不存在：${source}`);
  if (!existsSync(destination)) throw new Error(`${label} 未写入安装目录：${destination}`);
  const sourceBytes = readFileSync(source);
  const destinationBytes = readFileSync(destination);
  if (!sourceBytes.equals(destinationBytes)) {
    throw new Error(`${label} 安装副本与已验证源文件不一致`);
  }
}

function copyTree(source, destination) {
  if (!existsSync(source)) throw new Error(`安装资源不存在：${source}`);
  mkdirSync(dirname(destination), { recursive: true });
  cpSync(source, destination, {
    recursive: true,
    dereference: true,
    force: true,
  });
}

async function acquireWebviewBootstrapper(tempRoot) {
  const configured = process.env.PET_MANAGER_WEBVIEW2_BOOTSTRAPPER;
  const destination = join(tempRoot, "MicrosoftEdgeWebview2Setup.exe");
  if (configured) {
    copyFileSync(resolve(configured), destination);
  } else {
    const response = await fetch(webviewBootstrapperUrl, { redirect: "follow" });
    if (!response.ok) {
      throw new Error(
        `WebView2 bootstrapper 下载失败：HTTP ${response.status}`,
      );
    }
    writeFileSync(destination, Buffer.from(await response.arrayBuffer()));
  }
  assertPeExecutable(destination, "WebView2 bootstrapper");
  return destination;
}

function directorySize(path) {
  const result = spawnSync("du", ["-sk", path], { encoding: "utf8" });
  if (result.status !== 0) return 0;
  return Number.parseInt(result.stdout.trim().split(/\s+/)[0], 10) || 0;
}

function nsiPath(path) {
  return path.replaceAll("\\", "/").replaceAll("$", "$$").replaceAll('"', '$\\"');
}

assertPeExecutable(appExeSource, "Pet Manager Windows 主程序");
const windowsNode = join(tauriRoot, "generated-runtime", "node.exe");
assertPeExecutable(windowsNode, "Windows Node runtime");
const windowsFfmpeg = join(tauriRoot, "generated-runtime", "ffmpeg.exe");
assertPeExecutable(windowsFfmpeg, "Windows LGPL FFmpeg runtime");
const bundledFirmwareSource = join(
  tauriRoot,
  "firmware",
  "esp32-p4",
  "firmware.bin",
);
const bundledFirmwareDestination = join(
  stageRoot,
  "firmware",
  "esp32-p4",
  "firmware.bin",
);

rmSync(stageRoot, { recursive: true, force: true });
mkdirSync(stageRoot, { recursive: true });
copyFileSync(appExeSource, join(stageRoot, appExeName));
mkdirSync(dirname(bundledFirmwareDestination), { recursive: true });
copyFileSync(bundledFirmwareSource, bundledFirmwareDestination);
assertExactFileCopy(
  bundledFirmwareSource,
  bundledFirmwareDestination,
  "Windows 内置 P4 固件",
);
copyTree(join(refRoot, "dist", "terrier-clips"), join(stageRoot, "terrier-clips"));
copyTree(
  join(refRoot, "builtin-clawpkgs"),
  join(stageRoot, "builtin-clawpkgs"),
);
copyTree(
  join(tauriRoot, "bridge", "package.json"),
  join(stageRoot, "bridge", "package.json"),
);
copyTree(
  join(tauriRoot, "bridge", "packages", "clawd-backend-service", "package.json"),
  join(stageRoot, "bridge", "packages", "clawd-backend-service", "package.json"),
);
copyTree(
  join(tauriRoot, "bridge", "packages", "clawd-backend-service", "src"),
  join(stageRoot, "bridge", "packages", "clawd-backend-service", "src"),
);
copyTree(
  join(
    tauriRoot,
    "bridge",
    "packages",
    "clawd-backend-service",
    "node_modules",
  ),
  join(
    stageRoot,
    "bridge",
    "packages",
    "clawd-backend-service",
    "node_modules",
  ),
);
copyTree(
  join(tauriRoot, "bridge", "packages", "agent-session-bus", "package.json"),
  join(stageRoot, "bridge", "packages", "agent-session-bus", "package.json"),
);
copyTree(
  join(tauriRoot, "bridge", "packages", "agent-session-bus", "src"),
  join(stageRoot, "bridge", "packages", "agent-session-bus", "src"),
);
copyTree(
  join(tauriRoot, "bridge", "agents"),
  join(stageRoot, "bridge", "agents"),
);
copyTree(
  join(tauriRoot, "bridge", "hooks"),
  join(stageRoot, "bridge", "hooks"),
);
mkdirSync(join(stageRoot, "bridge", "runtime"), { recursive: true });
copyFileSync(windowsNode, join(stageRoot, "bridge", "runtime", "node.exe"));
mkdirSync(join(stageRoot, "tools"), { recursive: true });
copyFileSync(windowsFfmpeg, join(stageRoot, "tools", "ffmpeg.exe"));
for (const notice of ["ffmpeg.LICENSE", "ffmpeg.README", "ffmpeg.SOURCE.txt", "zlib.LICENSE"]) {
  copyFileSync(
    join(tauriRoot, "generated-runtime", notice),
    join(stageRoot, "tools", notice),
  );
}
mkdirSync(join(stageRoot, "tools", "source"), { recursive: true });
copyFileSync(
  join(tauriRoot, "generated-runtime", "ffmpeg-8.1.2.tar.xz"),
  join(stageRoot, "tools", "source", "ffmpeg-8.1.2.tar.xz"),
);
copyTree(
  join(repositoryRoot, "pc", "skills", "petui"),
  join(stageRoot, "skills", "petui"),
);

const requiredResources = [
  bundledFirmwareDestination,
  join(stageRoot, "bridge", "runtime", "node.exe"),
  join(stageRoot, "tools", "ffmpeg.exe"),
  join(stageRoot, "tools", "ffmpeg.LICENSE"),
  join(stageRoot, "tools", "ffmpeg.README"),
  join(stageRoot, "tools", "ffmpeg.SOURCE.txt"),
  join(stageRoot, "tools", "zlib.LICENSE"),
  join(stageRoot, "tools", "source", "ffmpeg-8.1.2.tar.xz"),
  join(
    stageRoot,
    "bridge",
    "packages",
    "clawd-backend-service",
    "node_modules",
    "mqtt",
    "package.json",
  ),
  join(
    stageRoot,
    "bridge",
    "packages",
    "clawd-backend-service",
    "node_modules",
    "ws",
    "package.json",
  ),
  join(stageRoot, "builtin-clawpkgs", "tomato-clock"),
  join(stageRoot, "builtin-clawpkgs", "drink-reminder"),
  join(stageRoot, "builtin-clawpkgs", "token-usage"),
  join(stageRoot, "skills", "petui", "SKILL.md"),
];
for (const path of requiredResources) {
  if (!existsSync(path)) throw new Error(`NSIS staging 缺少必要资源：${path}`);
}

if (!existsSync(stopOwnedRuntimesScript)) {
  throw new Error(`NSIS runtime cleanup helper is missing: ${stopOwnedRuntimesScript}`);
}

mkdirSync(bundleRoot, { recursive: true });
const tempRoot = mkdtempSync(join(tmpdir(), "pet-manager-nsis-"));
try {
  const webviewBootstrapper = await acquireWebviewBootstrapper(tempRoot);
  const scriptPath = join(tempRoot, "pet-manager-installer.nsi");
  const estimatedSizeKiB = directorySize(stageRoot);
  const iconPath = nsiPath(join(tauriRoot, "icons", "icon.ico"));
  const stagePath = nsiPath(stageRoot);
  const bootstrapperPath = nsiPath(webviewBootstrapper);
  const stopOwnedRuntimesPath = nsiPath(stopOwnedRuntimesScript);
  const outputPath = nsiPath(installerPath);
  const nsi = `Unicode true
SetCompressor /SOLID lzma
SetCompressorDictSize 64
ManifestDPIAware true
RequestExecutionLevel user

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define APPNAME "Pet Manager"
!define APPVERSION "${version}"
!define APPGUID "com.petmanager.desktop"
!define WEBVIEW2APPGUID "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
!define UNINSTALLKEY "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${"${APPGUID}"}"

!macro StopPetManagerOwnedRuntimes
  InitPluginsDir
  File "/oname=$PLUGINSDIR\\pet-manager-stop-owned-runtimes.ps1" "${stopOwnedRuntimesPath}"
  nsExec::ExecToStack '"$SYSDIR\\WindowsPowerShell\\v1.0\\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\\pet-manager-stop-owned-runtimes.ps1" -InstallDir "$INSTDIR"'
  Pop $1
  Pop $2
  ${"${If}"} $1 != 0
    MessageBox MB_ICONSTOP "Pet Manager background services are still running. Close Pet Manager and retry.$\\r$\\n$2"
    Abort
  ${"${EndIf}"}
!macroend

Name "${"${APPNAME}"}"
OutFile "${outputPath}"
InstallDir "$LOCALAPPDATA\\Programs\\${"${APPNAME}"}"
InstallDirRegKey HKCU "${"${UNINSTALLKEY}"}" "InstallLocation"
Icon "${iconPath}"
UninstallIcon "${iconPath}"
VIProductVersion "${version}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${"${APPNAME}"}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${version}"
VIAddVersionKey /LANG=1033 "CompanyName" "Pet Manager"
VIAddVersionKey /LANG=1033 "FileDescription" "Pet Manager Setup"

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\\${appExeName}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch Pet Manager"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Section "Pet Manager" SEC_APP
  ${"${IfNot}"} ${"${RunningX64}"}
    MessageBox MB_ICONSTOP "Pet Manager requires 64-bit Windows."
    Abort
  ${"${EndIf}"}

  SetRegView 64
  StrCpy $4 ""
  ReadRegStr $4 HKLM "SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\${"${WEBVIEW2APPGUID}"}" "pv"
  ${"${If}"} $4 == ""
    ReadRegStr $4 HKLM "SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\${"${WEBVIEW2APPGUID}"}" "pv"
  ${"${EndIf}"}
  ${"${If}"} $4 == ""
    ReadRegStr $4 HKCU "SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\${"${WEBVIEW2APPGUID}"}" "pv"
  ${"${EndIf}"}
  ${"${If}"} $4 == ""
    InitPluginsDir
    File "/oname=$PLUGINSDIR\\MicrosoftEdgeWebview2Setup.exe" "${bootstrapperPath}"
    DetailPrint "Installing Microsoft Edge WebView2 Runtime..."
    ExecWait '"$PLUGINSDIR\\MicrosoftEdgeWebview2Setup.exe" /silent /install' $1
    ${"${If}"} $1 != 0
      MessageBox MB_ICONSTOP "WebView2 Runtime installation failed (code $1). Check your internet connection and run setup again."
      Abort
    ${"${EndIf}"}
  ${"${EndIf}"}

  !insertmacro StopPetManagerOwnedRuntimes
  RMDir /r "$INSTDIR"
  SetOutPath "$INSTDIR"
  File /r "${stagePath}/*"
  WriteUninstaller "$INSTDIR\\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\\Pet Manager.lnk" "$INSTDIR\\${appExeName}"
  CreateShortcut "$DESKTOP\\Pet Manager.lnk" "$INSTDIR\\${appExeName}"

  WriteRegStr HKCU "${"${UNINSTALLKEY}"}" "DisplayName" "${"${APPNAME}"}"
  WriteRegStr HKCU "${"${UNINSTALLKEY}"}" "DisplayVersion" "${version}"
  WriteRegStr HKCU "${"${UNINSTALLKEY}"}" "DisplayIcon" "$INSTDIR\\${appExeName}"
  WriteRegStr HKCU "${"${UNINSTALLKEY}"}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${"${UNINSTALLKEY}"}" "Publisher" "Pet Manager"
  WriteRegStr HKCU "${"${UNINSTALLKEY}"}" "UninstallString" '"$INSTDIR\\Uninstall.exe"'
  WriteRegStr HKCU "${"${UNINSTALLKEY}"}" "QuietUninstallString" '"$INSTDIR\\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${"${UNINSTALLKEY}"}" "NoModify" 1
  WriteRegDWORD HKCU "${"${UNINSTALLKEY}"}" "NoRepair" 1
  WriteRegDWORD HKCU "${"${UNINSTALLKEY}"}" "EstimatedSize" ${estimatedSizeKiB}
SectionEnd

Section "Uninstall"
  SetRegView 64
  !insertmacro StopPetManagerOwnedRuntimes
  Delete "$SMPROGRAMS\\Pet Manager.lnk"
  Delete "$DESKTOP\\Pet Manager.lnk"
  DeleteRegKey HKCU "${"${UNINSTALLKEY}"}"
  RMDir /r "$INSTDIR"
SectionEnd
`;
  writeFileSync(scriptPath, nsi);
  const makensis = process.env.MAKENSIS_BIN || "makensis";
  const result = spawnSync(makensis, ["-V3", scriptPath], {
    stdio: "inherit",
  });
  if (result.error) throw result.error;
  if (result.status !== 0) {
    throw new Error(`makensis 失败，退出码 ${result.status}`);
  }
} finally {
  rmSync(tempRoot, { recursive: true, force: true });
}

if (!existsSync(installerPath) || statSync(installerPath).size < 1_000_000) {
  throw new Error(`NSIS 安装包未生成或大小异常：${installerPath}`);
}
console.log(`Windows NSIS installer ready: ${installerPath}`);
