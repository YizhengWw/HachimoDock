/**
 * [Input] Target desktop platform, relocatable target-compatible Node executable, audited LGPL-only FFmpeg runtime/notices, bridge runtime lock, built-in pet media, and release P4 application image.
 * [Output] Install-relative Node/FFmpeg/bridge resources plus a validated, preconverted built-in P4 ready pack and version-matched bundled firmware.
 * [Pos] Desktop packaging preflight shared by local builds and CI.
 * [Sync] If this file changes, update `scripts/.folder.md`.
 */

import { createHash } from "node:crypto";
import {
  chmodSync,
  copyFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { prepareBuiltInP4Ready } from "./prepare-p4-ready-assets.mjs";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDir, "../..");
const tauriRoot = join(repositoryRoot, "pc", "src-tauri");
const bridgeRuntimePackage = join(
  tauriRoot,
  "bridge",
  "packages",
  "clawd-backend-service",
);
const generatedRuntime = join(tauriRoot, "generated-runtime");
const bundledP4FirmwareDir = join(tauriRoot, "firmware", "esp32-p4");
const windowsP4FirmwareVersion = "0.7.49-p4";
const p4ProjectCmake = join(repositoryRoot, "firmware", "CMakeLists.txt");
const auditedFfmpegBinarySha256 = {
  "macos-arm64": "e340c148c720888a528e7bd2e7867da3b4bea91d1f6e508c9526111257ee0468",
  "windows-x64": "1cefca14ea713d5d7e7a1b948299142831185487f5efb14914d049ba9090d95f",
};

function requestedTarget(argv, platform) {
  const inline = argv.find((value) => value.startsWith("--target="));
  const targetIndex = argv.indexOf("--target");
  const raw = inline?.slice("--target=".length)
    || (targetIndex >= 0 ? argv[targetIndex + 1] : "")
    || ({ win32: "windows", darwin: "macos", linux: "linux" }[platform]);
  const aliases = {
    win32: "windows",
    win: "windows",
    windows: "windows",
    darwin: "macos",
    mac: "macos",
    macos: "macos",
    linux: "linux",
  };
  const normalized = aliases[String(raw || "").toLowerCase()];
  if (!normalized) {
    throw new Error(`不支持的桌面目标平台：${raw || "(empty)"}`);
  }
  return normalized;
}

function hostTarget(platform) {
  return { win32: "windows", darwin: "macos", linux: "linux" }[platform] || "";
}

function requestedArch(target, platform, arch) {
  const raw = process.env.PET_MANAGER_TARGET_ARCH
    || (target === "windows" ? "x64" : target === hostTarget(platform) ? arch : "");
  const aliases = {
    amd64: "x64",
    x86_64: "x64",
    x64: "x64",
    aarch64: "arm64",
    arm64: "arm64",
  };
  const normalized = aliases[String(raw || "").toLowerCase()];
  if (!normalized) {
    throw new Error(
      `无法确定 ${target} 目标架构；请设置 PET_MANAGER_TARGET_ARCH=x64|arm64`,
    );
  }
  if (target === "windows" && normalized !== "x64") {
    throw new Error("Pet Manager Windows 安装包当前只支持 x64");
  }
  return normalized;
}

function executableFormat(bytes) {
  const isPe = bytes.length >= 2 && bytes[0] === 0x4d && bytes[1] === 0x5a;
  const isElf = bytes.length >= 4
    && bytes[0] === 0x7f
    && bytes[1] === 0x45
    && bytes[2] === 0x4c
    && bytes[3] === 0x46;
  const magic = bytes.length >= 4 ? bytes.readUInt32BE(0) : 0;
  const isMachO = new Set([
    0xfeedface,
    0xcefaedfe,
    0xfeedfacf,
    0xcffaedfe,
    0xcafebabe,
    0xbebafeca,
  ]).has(magic);
  return { isPe, isElf, isMachO };
}

function assertMacosRuntimeIsRelocatable(runtimePath) {
  const result = spawnSync("/usr/bin/otool", ["-L", runtimePath], {
    encoding: "utf8",
    timeout: 10_000,
  });
  if (result.error || result.status !== 0) {
    throw new Error(
      `无法检查 ${basename(runtimePath)} 的 macOS 动态库依赖：${result.error?.message || result.stderr}`,
    );
  }

  const externalDependencies = result.stdout
    .split(/\r?\n/)
    .slice(1)
    .map((line) => line.trim().split(/\s+\(compatibility version/)[0])
    .filter(Boolean)
    .filter((dependency) => (
      !dependency.startsWith("/System/Library/")
      && !dependency.startsWith("/usr/lib/")
    ));
  if (externalDependencies.length === 0) return;

  throw new Error(
    `${basename(runtimePath)} 依赖未随应用打包的动态库，不能作为可分发的 macOS 运行时：${externalDependencies.join(", ")}。`
      + "请设置 PET_MANAGER_NODE_BIN 指向只依赖 /System/Library 或 /usr/lib 的独立二进制",
  );
}

function assertRuntimeMatchesTarget(runtimePath, target, arch) {
  const bytes = readFileSync(runtimePath);
  const prefix = bytes.subarray(0, 160).toString("utf8");
  if (prefix.startsWith("version https://git-lfs.github.com/spec/v1")) {
    throw new Error(`${basename(runtimePath)} 是 Git LFS 指针，不是可执行的 Node 运行时`);
  }

  const { isPe, isElf, isMachO } = executableFormat(bytes);
  const valid = target === "windows" ? isPe : target === "macos" ? isMachO : isElf;
  if (!valid) {
    throw new Error(`${basename(runtimePath)} 不是 ${target} 可执行文件，拒绝生成错误平台安装包`);
  }

  if (target === "windows" && arch === "x64" && bytes.length > 0x40) {
    const peOffset = bytes.readUInt32LE(0x3c);
    const machine = peOffset + 6 <= bytes.length
      ? bytes.readUInt16LE(peOffset + 4)
      : 0;
    if (machine !== 0x8664) {
      throw new Error(`${basename(runtimePath)} 不是 Windows x64 可执行文件`);
    }
  }

  if (target === "macos" && bytes.length >= 8) {
    const littleMagic = bytes.readUInt32LE(0);
    if (littleMagic === 0xfeedfacf || littleMagic === 0xfeedface) {
      const cpuType = bytes.readUInt32LE(4);
      const expectedCpuType = arch === "arm64" ? 0x0100000c : 0x01000007;
      if (cpuType !== expectedCpuType) {
        throw new Error(`${basename(runtimePath)} 不是 macOS ${arch} 可执行文件`);
      }
    }
    if (process.platform !== "darwin") {
      throw new Error("macOS 运行时依赖校验必须在 macOS 主机上执行");
    }
    assertMacosRuntimeIsRelocatable(runtimePath);
  }
}

function assertLgplFfmpegMetadata(readmePath, licensePath, target) {
  if (!existsSync(readmePath) || !existsSync(licensePath)) {
    throw new Error("FFmpeg 运行时必须同时提供构建说明和 LGPL 许可证");
  }
  const readme = readFileSync(readmePath, "utf8");
  const license = readFileSync(licensePath, "utf8");
  const expectedEncoder = target === "windows" ? "h264_mf" : "h264_videotoolbox";
  for (const required of ["--disable-gpl", "--disable-nonfree", expectedEncoder]) {
    if (!readme.includes(required)) {
      throw new Error(`FFmpeg 构建说明缺少安全约束 ${required}`);
    }
  }
  for (const forbidden of [
    "--enable-gpl",
    "--enable-nonfree",
    "--enable-libx264",
    "--enable-libopenh264",
  ]) {
    if (readme.includes(forbidden)) {
      throw new Error(`拒绝分发包含 ${forbidden} 的 FFmpeg 构建`);
    }
  }
  if (!license.includes("GNU LESSER GENERAL PUBLIC LICENSE")) {
    throw new Error("FFmpeg 许可证文件不是 LGPL 文本");
  }
}

function assertWindowsFfmpegIsStandalone(runtimePath) {
  if (process.platform !== "darwin") return;
  const objdump = [
    "/opt/homebrew/bin/x86_64-w64-mingw32-objdump",
    "/usr/local/bin/x86_64-w64-mingw32-objdump",
  ].find(existsSync);
  if (!objdump) {
    throw new Error("缺少 x86_64-w64-mingw32-objdump，无法验证 Windows FFmpeg 依赖");
  }
  const result = spawnSync(objdump, ["-p", runtimePath], {
    encoding: "utf8",
    timeout: 10_000,
    maxBuffer: 64 * 1024 * 1024,
  });
  if (result.error || result.status !== 0) {
    throw new Error(`无法检查 Windows FFmpeg 依赖：${result.error?.message || result.stderr}`);
  }
  const forbiddenDlls = ["libgcc", "libstdc++", "libwinpthread"]
    .filter((name) => result.stdout.toLowerCase().includes(name));
  if (forbiddenDlls.length > 0) {
    throw new Error(`Windows FFmpeg 依赖未打包的 MinGW DLL：${forbiddenDlls.join(", ")}`);
  }
}

function ensureBundledFfmpeg(target, arch) {
  const runtimeName = target === "windows" ? "ffmpeg.exe" : "ffmpeg";
  const targetRuntime = join(generatedRuntime, runtimeName);
  const targetReadme = join(generatedRuntime, "ffmpeg.README");
  const targetLicense = join(generatedRuntime, "ffmpeg.LICENSE");
  const targetSource = join(generatedRuntime, "ffmpeg.SOURCE.txt");
  const targetSourceArchive = join(generatedRuntime, "ffmpeg-8.1.2.tar.xz");
  const targetZlibLicense = join(generatedRuntime, "zlib.LICENSE");
  const configuredRuntime = process.env.PET_MANAGER_FFMPEG_BIN;

  if (configuredRuntime) {
    const configuredReadme = process.env.PET_MANAGER_FFMPEG_README;
    const configuredLicense = process.env.PET_MANAGER_FFMPEG_LICENSE;
    const configuredSourceArchive = process.env.PET_MANAGER_FFMPEG_SOURCE_ARCHIVE;
    const configuredZlibLicense = process.env.PET_MANAGER_ZLIB_LICENSE;
    if (
      !configuredReadme
      || !configuredLicense
      || !configuredSourceArchive
      || (target === "windows" && !configuredZlibLicense)
    ) {
      throw new Error(
        "PET_MANAGER_FFMPEG_BIN 必须同时提供 README、LICENSE、SOURCE_ARCHIVE（Windows 另需 PET_MANAGER_ZLIB_LICENSE）",
      );
    }
    const sourceRuntime = resolve(configuredRuntime);
    const sourceReadme = resolve(configuredReadme);
    const sourceLicense = resolve(configuredLicense);
    const sourceArchive = resolve(configuredSourceArchive);
    if (!existsSync(sourceRuntime)) throw new Error(`FFmpeg 运行时不存在：${sourceRuntime}`);
    if (!existsSync(sourceArchive)) throw new Error(`FFmpeg 源码包不存在：${sourceArchive}`);
    const sourceArchiveHash = sha256(readFileSync(sourceArchive));
    if (sourceArchiveHash !== "464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c") {
      throw new Error(`FFmpeg 8.1.2 源码包 SHA-256 不匹配：${sourceArchiveHash}`);
    }
    assertRuntimeMatchesTarget(sourceRuntime, target, arch);
    assertLgplFfmpegMetadata(sourceReadme, sourceLicense, target);
    copyFileSync(sourceRuntime, targetRuntime);
    copyFileSync(sourceReadme, targetReadme);
    copyFileSync(sourceLicense, targetLicense);
    copyFileSync(sourceArchive, targetSourceArchive);
    if (target === "windows") copyFileSync(resolve(configuredZlibLicense), targetZlibLicense);
    writeFileSync(
      targetSource,
      [
        "Pet Manager bundled LGPL-only FFmpeg runtime",
        "FFmpeg version: 8.1.2",
        `Target: ${target}-${arch}`,
        "Source: https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz",
        "Source SHA-256: 464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c",
        `Binary SHA-256: ${sha256(readFileSync(targetRuntime))}`,
        "Relinkable FFmpeg source and exact build configuration are identified in ffmpeg.README.",
        "",
      ].join("\n"),
    );
  }

  if (![targetRuntime, targetReadme, targetLicense, targetSource, targetSourceArchive].every(existsSync)) {
    throw new Error(
      `缺少 ${target}-${arch} LGPL FFmpeg 运行时或对应源码；请提供 PET_MANAGER_FFMPEG_BIN/README/LICENSE/SOURCE_ARCHIVE`,
    );
  }
  if (target === "windows" && !existsSync(targetZlibLicense)) {
    throw new Error("Windows FFmpeg 缺少 zlib 许可证");
  }
  if (sha256(readFileSync(targetSourceArchive)) !== "464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c") {
    throw new Error("已暂存 FFmpeg 源码包校验失败");
  }
  const expectedBinaryHash = auditedFfmpegBinarySha256[`${target}-${arch}`];
  if (!expectedBinaryHash) {
    throw new Error(`没有 ${target}-${arch} 的已审计 FFmpeg 二进制哈希`);
  }
  const actualBinaryHash = sha256(readFileSync(targetRuntime));
  if (actualBinaryHash !== expectedBinaryHash) {
    throw new Error(
      `FFmpeg 二进制不是已审计构建：期望 ${expectedBinaryHash}，实际 ${actualBinaryHash}`,
    );
  }
  assertRuntimeMatchesTarget(targetRuntime, target, arch);
  assertLgplFfmpegMetadata(targetReadme, targetLicense, target);
  if (target === "windows") assertWindowsFfmpegIsStandalone(targetRuntime);
  if (target !== "windows") chmodSync(targetRuntime, 0o755);

  if (target === hostTarget(process.platform)) {
    const version = spawnSync(targetRuntime, ["-hide_banner", "-version"], {
      encoding: "utf8",
      timeout: 10_000,
    });
    const encoders = spawnSync(targetRuntime, ["-hide_banner", "-encoders"], {
      encoding: "utf8",
      timeout: 10_000,
    });
    const expectedEncoder = target === "windows" ? "h264_mf" : "h264_videotoolbox";
    const buildText = `${version.stdout || ""}\n${version.stderr || ""}`;
    if (
      version.error
      || version.status !== 0
      || !buildText.includes("--disable-gpl")
      || !buildText.includes("--disable-nonfree")
      || buildText.includes("--enable-gpl")
      || buildText.includes("--enable-nonfree")
      || encoders.status !== 0
      || !String(encoders.stdout).includes(expectedEncoder)
    ) {
      throw new Error(`FFmpeg 运行时安全探测失败：${version.error?.message || version.stderr}`);
    }
  }
  return runtimeName;
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function readNullTerminatedUtf8(bytes) {
  const end = bytes.indexOf(0);
  return bytes.subarray(0, end < 0 ? bytes.length : end).toString("utf8").trim();
}

function readFirmwareBuildId(firmware, version) {
  const prefix = Buffer.from(`${version}+`, "utf8");
  const offset = firmware.indexOf(prefix);
  if (offset < 0) return "";
  return readNullTerminatedUtf8(firmware.subarray(offset, offset + 128));
}

function validateBundledP4Firmware(target) {
  const firmwareName = target === "windows" ? "firmware.bin" : "firmware-macos.bin";
  const bundledP4Firmware = join(bundledP4FirmwareDir, firmwareName);
  if (!existsSync(bundledP4Firmware)) {
    throw new Error(`缺少 PC 内置 P4 固件：${bundledP4Firmware}`);
  }
  const firmware = readFileSync(bundledP4Firmware);
  const descriptorOffset = 24 + 8;
  if (firmware.length < descriptorOffset + 256 || firmware[0] !== 0xe9) {
    throw new Error("PC 内置 P4 固件不是有效的 ESP-IDF application image");
  }
  const magic = firmware.readUInt32LE(descriptorOffset);
  if (magic !== 0xabcd5432) {
    throw new Error("PC 内置 P4 固件缺少有效的 ESP-IDF app descriptor");
  }
  const version = readNullTerminatedUtf8(firmware.subarray(descriptorOffset + 16, descriptorOffset + 48));
  const projectName = readNullTerminatedUtf8(firmware.subarray(descriptorOffset + 48, descriptorOffset + 80));
  const cmake = readFileSync(p4ProjectCmake, "utf8");
  const sourceVersion = cmake.match(/set\(PROJECT_VER\s+"([^"]+)"\)/)?.[1] || "";
  const expectedVersion = target === "windows" ? windowsP4FirmwareVersion : sourceVersion;
  if (projectName !== "pet_manager_p4_runtime") {
    throw new Error(`PC 内置固件 projectName 异常：${projectName || "(empty)"}`);
  }
  if (!expectedVersion || version !== expectedVersion) {
    throw new Error(`PC 内置固件版本 ${version || "(empty)"} 与源码版本 ${expectedVersion || "(empty)"} 不一致`);
  }
  const buildId = readFirmwareBuildId(firmware, version);
  const cleanBuildPattern = new RegExp(`^${version.replaceAll(".", "\\.")}\\+[0-9a-f]{12}$`);
  if (!cleanBuildPattern.test(buildId)) {
    throw new Error(
      `PC 内置固件必须来自干净的 12 位 Git 提交，当前 buildId：${buildId || "(missing)"}`,
    );
  }
  return { version, buildId, bytes: firmware.length, sha256: sha256(firmware) };
}

function ensureBridgeDependencies() {
  const mqttPackage = join(bridgeRuntimePackage, "node_modules", "mqtt", "package.json");
  const wsPackage = join(bridgeRuntimePackage, "node_modules", "ws", "package.json");
  if (existsSync(mqttPackage) && existsSync(wsPackage)) return;

  const npmArgs = ["ci", "--omit=dev", "--ignore-scripts", "--no-audit", "--no-fund"];
  const npmExecPath = process.env.npm_execpath;
  const result = npmExecPath
    ? spawnSync(process.execPath, [npmExecPath, ...npmArgs], {
        cwd: bridgeRuntimePackage,
        stdio: "inherit",
      })
    : spawnSync("npm", npmArgs, {
        cwd: bridgeRuntimePackage,
        stdio: "inherit",
        shell: process.platform === "win32",
      });
  if (result.error) throw result.error;
  if (result.status !== 0) {
    throw new Error(`bridge 生产依赖安装失败，npm 退出码 ${result.status}`);
  }
}

const target = requestedTarget(process.argv.slice(2), process.platform);
const targetArch = requestedArch(target, process.platform, process.arch);
const runtimeName = target === "windows" ? "node.exe" : "node";
const targetRuntime = join(generatedRuntime, runtimeName);
const p4Firmware = validateBundledP4Firmware(target);
let sourceRuntime = process.env.PET_MANAGER_NODE_BIN
  ? resolve(process.env.PET_MANAGER_NODE_BIN)
  : process.execPath;

// Once a relocatable runtime has passed preflight, keep reusing that exact
// staged binary for later local rebuilds. This prevents a Homebrew/npm PATH
// change from silently changing or breaking the packaged runtime. A clean
// checkout still fails closed unless the active Node is relocatable or the
// caller supplies PET_MANAGER_NODE_BIN.
if (!process.env.PET_MANAGER_NODE_BIN && existsSync(targetRuntime)) {
  try {
    assertRuntimeMatchesTarget(targetRuntime, target, targetArch);
    sourceRuntime = targetRuntime;
    console.log(`reusing validated staged Node runtime: ${targetRuntime}`);
  } catch {
    // Preserve the detailed validation error for the active runtime below.
  }
}

if (!process.env.PET_MANAGER_NODE_BIN && target !== hostTarget(process.platform)) {
  throw new Error(
    `当前主机 Node 不能用于 ${target}。请设置 PET_MANAGER_NODE_BIN 指向目标平台的 Node 可执行文件`,
  );
}
if (!existsSync(sourceRuntime)) {
  throw new Error(`Node 运行时不存在：${sourceRuntime}`);
}

assertRuntimeMatchesTarget(sourceRuntime, target, targetArch);
ensureBridgeDependencies();
mkdirSync(generatedRuntime, { recursive: true });

if (resolve(sourceRuntime) !== resolve(targetRuntime)) {
  copyFileSync(sourceRuntime, targetRuntime);
}
if (target !== "windows") chmodSync(targetRuntime, 0o755);

const ffmpegRuntimeName = ensureBundledFfmpeg(target, targetArch);

const buildHostFfmpeg = process.env.PET_MANAGER_BUILD_FFMPEG_BIN || "ffmpeg";
const p4Ready = prepareBuiltInP4Ready({
  ffmpegPath: target === hostTarget(process.platform)
    ? buildHostFfmpeg
    : "",
  allowTranscode: target === hostTarget(process.platform),
});

console.log(
  `desktop resources ready: ${target}-${targetArch}, generated-runtime/${runtimeName}, generated-runtime/${ffmpegRuntimeName} (LGPL-only system H.264), p4-ready/${p4Ready.profile} (${p4Ready.bytes} bytes), firmware/${p4Firmware.buildId} (${p4Firmware.bytes} bytes, sha256 ${p4Firmware.sha256})`,
);
