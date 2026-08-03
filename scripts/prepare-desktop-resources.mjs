/**
 * [Input] Target desktop platform, target-compatible Node/FFmpeg executables, bridge runtime lock, and built-in pet media.
 * [Output] Install-relative Node/FFmpeg/bridge resources plus a validated, preconverted built-in P4 ready pack.
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
import { gunzipSync, inflateRawSync } from "node:zlib";
import { prepareBuiltInP4Ready } from "./prepare-p4-ready-assets.mjs";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDir, "..");
const tauriRoot = join(repositoryRoot, "ref", "src-tauri");
const bridgeRuntimePackage = join(
  tauriRoot,
  "bridge",
  "packages",
  "clawd-backend-service",
);
const generatedRuntime = join(tauriRoot, "generated-runtime");
const ffmpegStaticRelease = "b6.1.1";
const ffmpegStaticBaseUrl =
  `https://github.com/eugeneware/ffmpeg-static/releases/download/${ffmpegStaticRelease}`;
const ffmpegBuilds = {
  "macos-arm64": {
    release: "FFmpeg 8.1.2 macOS arm64",
    archiveFormat: "zip",
    archiveUrl:
      "https://ffmpeg.martin-riedl.de/download/macos/arm64/1783011502_8.1.2/ffmpeg.zip",
    readmeUrl:
      "https://ffmpeg.martin-riedl.de/download/macos/arm64/1783011502_8.1.2/versions.txt",
    licenseUrl:
      "https://raw.githubusercontent.com/FFmpeg/FFmpeg/n8.1.2/COPYING.GPLv3",
    sourceUrl: "https://github.com/FFmpeg/FFmpeg/tree/n8.1.2",
    distributorUrl: "https://git.martin-riedl.de/ffmpeg/build-script",
    archiveSha256: "ef1aa60006c7b77ce170c1608c08d8e4ba1c30c5746f2ac986ded932d0ac2c3c",
    binarySha256: "eaf91238e104dd0e262bc6510e25061855cc99a6955a721b0ac99660d58c473d",
  },
  "macos-x64": {
    release: "FFmpeg 8.1.2 macOS x64",
    archiveFormat: "zip",
    archiveUrl:
      "https://ffmpeg.martin-riedl.de/download/macos/amd64/1783018342_8.1.2/ffmpeg.zip",
    readmeUrl:
      "https://ffmpeg.martin-riedl.de/download/macos/amd64/1783018342_8.1.2/versions.txt",
    licenseUrl:
      "https://raw.githubusercontent.com/FFmpeg/FFmpeg/n8.1.2/COPYING.GPLv3",
    sourceUrl: "https://github.com/FFmpeg/FFmpeg/tree/n8.1.2",
    distributorUrl: "https://git.martin-riedl.de/ffmpeg/build-script",
    archiveSha256: "a52ef43883f44c219766d4b3bdde4e635b35465d0b704c01c3a0566b59775df9",
    binarySha256: "1ca59dda73668c59898a0b305afd8a88817a989187f222ec62d64e775d614d23",
  },
  "windows-x64": {
    release: `ffmpeg-static ${ffmpegStaticRelease} / FFmpeg 6.1.1 Windows x64`,
    archiveFormat: "gzip",
    archiveUrl: `${ffmpegStaticBaseUrl}/ffmpeg-win32-x64.gz`,
    readmeUrl: `${ffmpegStaticBaseUrl}/win32-x64.README`,
    licenseUrl: `${ffmpegStaticBaseUrl}/win32-x64.LICENSE`,
    sourceUrl: "https://github.com/FFmpeg/FFmpeg/commit/e38092ef93",
    distributorUrl: "https://github.com/eugeneware/ffmpeg-static/tree/5.3.0",
    archiveSha256: "8883a3dffbd0a16cf4ef95206ea05283f78908dbfb118f73c83f4951dcc06d77",
    binarySha256: "04e1307997530f9cf2fe35cba2ca7e8875ca91da02f89d6c7243df819c94ad00",
  },
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
  }
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

async function downloadBytes(url, label) {
  const response = await fetch(url, { redirect: "follow" });
  if (!response.ok) {
    throw new Error(`${label} 下载失败：HTTP ${response.status}`);
  }
  return Buffer.from(await response.arrayBuffer());
}

function extractZipEntry(archive, expectedName) {
  let eocdOffset = -1;
  for (let offset = archive.length - 22; offset >= Math.max(0, archive.length - 65_557); offset -= 1) {
    if (archive.readUInt32LE(offset) === 0x06054b50) {
      eocdOffset = offset;
      break;
    }
  }
  if (eocdOffset < 0) throw new Error("FFmpeg ZIP 缺少中央目录");

  const entryCount = archive.readUInt16LE(eocdOffset + 10);
  let centralOffset = archive.readUInt32LE(eocdOffset + 16);
  for (let index = 0; index < entryCount; index += 1) {
    if (archive.readUInt32LE(centralOffset) !== 0x02014b50) {
      throw new Error("FFmpeg ZIP 中央目录损坏");
    }
    const compression = archive.readUInt16LE(centralOffset + 10);
    const compressedSize = archive.readUInt32LE(centralOffset + 20);
    const uncompressedSize = archive.readUInt32LE(centralOffset + 24);
    const nameLength = archive.readUInt16LE(centralOffset + 28);
    const extraLength = archive.readUInt16LE(centralOffset + 30);
    const commentLength = archive.readUInt16LE(centralOffset + 32);
    const localOffset = archive.readUInt32LE(centralOffset + 42);
    const name = archive
      .subarray(centralOffset + 46, centralOffset + 46 + nameLength)
      .toString("utf8");

    if (name === expectedName) {
      if (archive.readUInt32LE(localOffset) !== 0x04034b50) {
        throw new Error("FFmpeg ZIP 本地文件头损坏");
      }
      const localNameLength = archive.readUInt16LE(localOffset + 26);
      const localExtraLength = archive.readUInt16LE(localOffset + 28);
      const dataOffset = localOffset + 30 + localNameLength + localExtraLength;
      const compressed = archive.subarray(dataOffset, dataOffset + compressedSize);
      const binary = compression === 0
        ? Buffer.from(compressed)
        : compression === 8
          ? inflateRawSync(compressed)
          : null;
      if (!binary || binary.length !== uncompressedSize) {
        throw new Error(`FFmpeg ZIP 不支持压缩方式 ${compression} 或文件大小异常`);
      }
      return binary;
    }

    centralOffset += 46 + nameLength + extraLength + commentLength;
  }
  throw new Error(`FFmpeg ZIP 中缺少 ${expectedName}`);
}

function extractFfmpegBinary(archive, build) {
  if (build.archiveFormat === "gzip") return gunzipSync(archive);
  if (build.archiveFormat === "zip") return extractZipEntry(archive, "ffmpeg");
  throw new Error(`不支持的 FFmpeg 压缩格式：${build.archiveFormat}`);
}

function assertRedistributableFfmpeg(readme) {
  if (readme.toString("utf8").includes("--enable-nonfree")) {
    throw new Error("FFmpeg 构建包含 --enable-nonfree，依法不可随安装包分发");
  }
}

async function ensureBundledFfmpeg(target, arch) {
  const runtimeName = target === "windows" ? "ffmpeg.exe" : "ffmpeg";
  const targetRuntime = join(generatedRuntime, runtimeName);
  const targetReadme = join(generatedRuntime, "ffmpeg.README");
  const targetLicense = join(generatedRuntime, "ffmpeg.LICENSE");
  const targetSource = join(generatedRuntime, "ffmpeg.SOURCE.txt");
  const configuredRuntime = process.env.PET_MANAGER_FFMPEG_BIN;

  if (configuredRuntime) {
    const sourceRuntime = resolve(configuredRuntime);
    if (!existsSync(sourceRuntime)) {
      throw new Error(`FFmpeg 运行时不存在：${sourceRuntime}`);
    }
    const configuredReadme = process.env.PET_MANAGER_FFMPEG_README;
    const configuredLicense = process.env.PET_MANAGER_FFMPEG_LICENSE;
    if (!configuredReadme || !configuredLicense) {
      throw new Error(
        "自定义 PET_MANAGER_FFMPEG_BIN 必须同时提供 PET_MANAGER_FFMPEG_README 与 PET_MANAGER_FFMPEG_LICENSE",
      );
    }
    assertRuntimeMatchesTarget(sourceRuntime, target, arch);
    copyFileSync(sourceRuntime, targetRuntime);
    copyFileSync(resolve(configuredReadme), targetReadme);
    copyFileSync(resolve(configuredLicense), targetLicense);
    assertRedistributableFfmpeg(readFileSync(targetReadme));
    writeFileSync(
      targetSource,
      [
        "Pet Manager bundled FFmpeg",
        `Target: ${target}-${arch}`,
        `Binary SHA-256: ${sha256(readFileSync(targetRuntime))}`,
        "Binary, source, configuration, and license details are documented in ffmpeg.README and ffmpeg.LICENSE.",
        "",
      ].join("\n"),
    );
  } else {
    const build = ffmpegBuilds[`${target}-${arch}`];
    if (!build) {
      throw new Error(
        `没有 ${target}-${arch} 的内置 FFmpeg 构建；请设置 PET_MANAGER_FFMPEG_BIN 及其 README/LICENSE`,
      );
    }
    const cachedBinary = existsSync(targetRuntime)
      && sha256(readFileSync(targetRuntime)) === build.binarySha256;
    const cachedNotices = existsSync(targetReadme)
      && existsSync(targetLicense)
      && existsSync(targetSource);

    if (!cachedBinary || !cachedNotices) {
      const archive = await downloadBytes(build.archiveUrl, "FFmpeg");
      const archiveHash = sha256(archive);
      if (archiveHash !== build.archiveSha256) {
        throw new Error(
          `FFmpeg 压缩包校验失败：期望 ${build.archiveSha256}，实际 ${archiveHash}`,
        );
      }
      const binary = extractFfmpegBinary(archive, build);
      const binaryHash = sha256(binary);
      if (binaryHash !== build.binarySha256) {
        throw new Error(
          `FFmpeg 二进制校验失败：期望 ${build.binarySha256}，实际 ${binaryHash}`,
        );
      }
      const [readme, license] = await Promise.all([
        downloadBytes(build.readmeUrl, "FFmpeg README"),
        downloadBytes(build.licenseUrl, "FFmpeg LICENSE"),
      ]);
      assertRedistributableFfmpeg(readme);
      writeFileSync(targetRuntime, binary);
      writeFileSync(targetReadme, readme);
      writeFileSync(targetLicense, license);
      writeFileSync(
        targetSource,
        [
          "Pet Manager bundled FFmpeg",
          `Release: ${build.release}`,
          `Target: ${target}-${arch}`,
          `Binary asset: ${build.archiveUrl}`,
          `Archive SHA-256: ${build.archiveSha256}`,
          `Binary SHA-256: ${build.binarySha256}`,
          `FFmpeg source: ${build.sourceUrl}`,
          `Distributor source and build metadata: ${build.distributorUrl}`,
          "License and exact build details are included beside this file.",
          "",
        ].join("\n"),
      );
    }
    assertRedistributableFfmpeg(readFileSync(targetReadme));
  }

  if (target !== "windows") chmodSync(targetRuntime, 0o755);
  assertRuntimeMatchesTarget(targetRuntime, target, arch);

  if (target === hostTarget(process.platform)) {
    const result = spawnSync(targetRuntime, ["-version"], {
      encoding: "utf8",
      timeout: 10_000,
    });
    if (result.error || result.status !== 0 || !result.stdout.includes("ffmpeg version")) {
      throw new Error(`内置 FFmpeg 无法在当前主机运行：${result.error?.message || result.stderr}`);
    }
    assertRedistributableFfmpeg(Buffer.from(`${result.stdout}\n${result.stderr || ""}`));
  }

  return runtimeName;
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
const sourceRuntime = process.env.PET_MANAGER_NODE_BIN
  ? resolve(process.env.PET_MANAGER_NODE_BIN)
  : process.execPath;

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

const runtimeName = target === "windows" ? "node.exe" : "node";
const targetRuntime = join(generatedRuntime, runtimeName);
copyFileSync(sourceRuntime, targetRuntime);
if (target !== "windows") chmodSync(targetRuntime, 0o755);

const ffmpegRuntimeName = await ensureBundledFfmpeg(target, targetArch);
const p4Ready = prepareBuiltInP4Ready({
  ffmpegPath: target === hostTarget(process.platform)
    ? join(generatedRuntime, ffmpegRuntimeName)
    : "",
  allowTranscode: target === hostTarget(process.platform),
});

console.log(
  `desktop resources ready: ${target}-${targetArch}, generated-runtime/${runtimeName}, generated-runtime/${ffmpegRuntimeName}, p4-ready/${p4Ready.profile} (${p4Ready.bytes} bytes)`,
);
