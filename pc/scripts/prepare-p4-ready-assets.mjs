/**
 * [Input] Built-in Terrier MP4/WAV sources, the source-matched pinned ready cache, the shared P4 H.264 appearance profile, and a host FFmpeg binary.
 * [Output] A deterministic, SPS-compatible p4-ready pack that reuses validated source-matched video/audio and is shared by Pet Manager and factory images.
 * [Pos] Desktop/factory asset prebuild shared by local packaging and CI.
 * [Sync] If this file changes, update `scripts/.folder.md`.
 */

import { createHash } from "node:crypto";
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { basename, dirname, extname, join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDir, "../..");
const runtimeRoot = join(repositoryRoot, "firmware");
const configPath = join(runtimeRoot, "factory-config.json");
const config = JSON.parse(readFileSync(configPath, "utf8"));
const appearance = config.appearance;
const clipsDir = resolve(runtimeRoot, appearance.clipsDir);
const profile = appearance.readyProfile
  || `v6-${appearance.width}x${appearance.height}-${appearance.fps}fps-${appearance.maxFrames}f-h264-crf${appearance.h264Crf}`;
const readyBase = join(clipsDir, "p4-ready");
const profileRoot = join(readyBase, profile);
const metadataName = "ready-meta.json";

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function assertNotGitLfsPointer(bytes, assetPath) {
  if (bytes.subarray(0, 160).toString("utf8").startsWith("version https://git-lfs.github.com/spec/v1")) {
    throw new Error(`${assetPath} 是 Git LFS 指针；请先安装 Git LFS 并运行 git lfs pull`);
  }
}

function u64le(value) {
  const bytes = Buffer.alloc(8);
  bytes.writeBigUInt64LE(BigInt(value));
  return bytes;
}

function stableJson(value) {
  if (value === null || typeof value !== "object") return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map(stableJson).join(",")}]`;
  return `{${Object.keys(value)
    .sort()
    .map((key) => `${JSON.stringify(key)}:${stableJson(value[key])}`)
    .join(",")}}`;
}

function findAnnexBNals(bytes) {
  const starts = [];
  for (let cursor = 0; cursor + 3 <= bytes.length;) {
    if (cursor + 4 <= bytes.length
        && bytes[cursor] === 0
        && bytes[cursor + 1] === 0
        && bytes[cursor + 2] === 0
        && bytes[cursor + 3] === 1) {
      starts.push({ start: cursor, prefixSize: 4 });
      cursor += 4;
    } else if (bytes[cursor] === 0 && bytes[cursor + 1] === 0 && bytes[cursor + 2] === 1) {
      starts.push({ start: cursor, prefixSize: 3 });
      cursor += 3;
    } else {
      cursor += 1;
    }
  }
  return starts.map((entry, index) => ({
    ...entry,
    end: index + 1 < starts.length ? starts[index + 1].start : bytes.length,
  }));
}

class BitWriter {
  constructor() {
    this.bits = [];
  }

  bit(value) {
    this.bits.push(value & 1);
  }

  fixed(value, count) {
    for (let shift = count - 1; shift >= 0; shift -= 1) this.bit(value >> shift);
  }

  ue(value) {
    const codeNum = value + 1;
    const leadingZeroes = Math.floor(Math.log2(codeNum));
    for (let index = 0; index < leadingZeroes; index += 1) this.bit(0);
    this.fixed(codeNum, leadingZeroes + 1);
  }

  finishRbsp() {
    this.bit(1);
    while (this.bits.length % 8) this.bit(0);
    const output = Buffer.alloc(this.bits.length / 8);
    for (let offset = 0; offset < this.bits.length; offset += 8) {
      let value = 0;
      for (const bit of this.bits.slice(offset, offset + 8)) value = (value << 1) | bit;
      output[offset / 8] = value;
    }
    return output;
  }
}

function addEmulationPrevention(rbsp) {
  const output = [];
  let zeroCount = 0;
  for (const value of rbsp) {
    if (zeroCount >= 2 && value <= 3) {
      output.push(3);
      zeroCount = 0;
    }
    output.push(value);
    zeroCount = value === 0 ? zeroCount + 1 : 0;
  }
  return Buffer.from(output);
}

function buildP4MinimalSps(width, height) {
  if (width <= 0 || height <= 0 || width % 2 || height % 2) {
    throw new Error("P4 H.264 width and height must be positive even values");
  }
  const encodedWidth = Math.ceil(width / 16) * 16;
  const encodedHeight = Math.ceil(height / 16) * 16;
  const cropRight = (encodedWidth - width) / 2;
  const cropBottom = (encodedHeight - height) / 2;
  const hasCrop = cropRight !== 0 || cropBottom !== 0;
  const writer = new BitWriter();
  writer.ue(0); // seq_parameter_set_id
  writer.ue(0); // log2_max_frame_num_minus4
  writer.ue(2); // pic_order_cnt_type
  writer.ue(1); // max_num_ref_frames
  writer.bit(0); // gaps_in_frame_num_value_allowed_flag
  writer.ue(encodedWidth / 16 - 1);
  writer.ue(encodedHeight / 16 - 1);
  writer.bit(1); // frame_mbs_only_flag
  writer.bit(0); // direct_8x8_inference_flag
  writer.bit(hasCrop ? 1 : 0);
  if (hasCrop) {
    writer.ue(0);
    writer.ue(cropRight);
    writer.ue(0);
    writer.ue(cropBottom);
  }
  writer.bit(0); // vui_parameters_present_flag
  const rbsp = Buffer.concat([Buffer.from([66, 0xc0, 30]), writer.finishRbsp()]);
  return Buffer.concat([Buffer.from([0x67]), addEmulationPrevention(rbsp)]);
}

function rewriteH264Sps(bytes, width, height) {
  const replacement = buildP4MinimalSps(width, height);
  const parts = [];
  let cursor = 0;
  let replaced = 0;
  for (const nal of findAnnexBNals(bytes)) {
    const payloadStart = nal.start + nal.prefixSize;
    if (payloadStart >= nal.end || (bytes[payloadStart] & 0x1f) !== 7) continue;
    parts.push(bytes.subarray(cursor, payloadStart), replacement);
    cursor = nal.end;
    replaced += 1;
  }
  if (replaced === 0) throw new Error("P4 H.264 stream contains no SPS");
  parts.push(bytes.subarray(cursor));
  return Buffer.concat(parts);
}

function parseH264Stream(bytes) {
  let accessUnits = 0;
  let slicesInAccessUnit = 0;
  let hasSps = false;
  let hasPps = false;
  for (const nal of findAnnexBNals(bytes)) {
    const payloadStart = nal.start + nal.prefixSize;
    if (payloadStart >= nal.end) continue;
    const type = bytes[payloadStart] & 0x1f;
    if (type === 9) {
      if (accessUnits > 0 && slicesInAccessUnit !== 1) {
        throw new Error("P4 H.264 stream must contain exactly one slice per access unit");
      }
      accessUnits += 1;
      slicesInAccessUnit = 0;
    } else if (type === 1 || type === 5) {
      if (accessUnits === 0) {
        throw new Error("P4 H.264 stream is missing access unit delimiters");
      }
      slicesInAccessUnit += 1;
      if (slicesInAccessUnit > 1) {
        throw new Error("P4 H.264 stream must contain exactly one slice per access unit");
      }
    }
    else if (type === 7) hasSps = true;
    else if (type === 8) hasPps = true;
  }
  if (accessUnits === 0 || slicesInAccessUnit !== 1) {
    throw new Error("P4 H.264 stream must contain exactly one slice per access unit");
  }
  if (!hasSps || !hasPps) {
    throw new Error("P4 H.264 stream is missing SPS, PPS, or video frames");
  }
  return { frames: accessUnits, streamBytes: bytes.length };
}

function wavMatchesDeviceContract(bytes, sampleRate) {
  if (bytes.length < 12 || bytes.toString("ascii", 0, 4) !== "RIFF"
      || bytes.toString("ascii", 8, 12) !== "WAVE") return false;
  let offset = 12;
  let formatOk = false;
  let dataOk = false;
  while (offset + 8 <= bytes.length) {
    const id = bytes.toString("ascii", offset, offset + 4);
    const size = bytes.readUInt32LE(offset + 4);
    const dataOffset = offset + 8;
    const end = dataOffset + size;
    if (end > bytes.length) return false;
    if (id === "fmt " && size >= 16) {
      formatOk = bytes.readUInt16LE(dataOffset) === 1
        && bytes.readUInt16LE(dataOffset + 2) === 1
        && bytes.readUInt32LE(dataOffset + 4) === sampleRate
        && bytes.readUInt16LE(dataOffset + 14) === 16;
    } else if (id === "data") {
      dataOk = size > 0 && size % 2 === 0;
    }
    offset = end + (size & 1);
  }
  return formatOk && dataOk;
}

function runFfmpeg(ffmpegPath, args, label) {
  if (!ffmpegPath) throw new Error(`P4 ready asset ${label} is stale and no host FFmpeg was provided`);
  const result = spawnSync(ffmpegPath, args, {
    encoding: "utf8",
    timeout: 120_000,
    windowsHide: true,
  });
  if (result.error || result.status !== 0) {
    throw new Error(
      `FFmpeg failed for ${label}: ${result.error?.message || result.stderr?.trim() || `exit ${result.status}`}`,
    );
  }
}

function parseFfmpegContainerDurationMs(stderr) {
  const match = String(stderr || "").match(
    /Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)/,
  );
  if (!match) return 0;
  const durationMs = Math.round(
    (Number(match[1]) * 3600 + Number(match[2]) * 60 + Number(match[3])) * 1000,
  );
  return Number.isFinite(durationMs) && durationMs > 0 ? durationMs : 0;
}

function probeVideoDurationMs(ffmpegPath, sourcePath, label) {
  if (!ffmpegPath) throw new Error(`P4 ready asset ${label} is stale and no host FFmpeg was provided`);
  const result = spawnSync(
    ffmpegPath,
    [
      "-hide_banner",
      "-nostdin",
      "-i",
      sourcePath,
      "-map",
      "0:v:0",
      "-c",
      "copy",
      "-f",
      "null",
      "-",
      "-progress",
      "pipe:1",
      "-nostats",
    ],
    {
      encoding: "utf8",
      timeout: 120_000,
      windowsHide: true,
    },
  );
  if (result.error || result.status !== 0) {
    throw new Error(
      `FFmpeg duration probe failed for ${label}: ${result.error?.message || result.stderr?.trim() || `exit ${result.status}`}`,
    );
  }
  const containerDurationMs = parseFfmpegContainerDurationMs(result.stderr);
  if (containerDurationMs > 0) return containerDurationMs;

  const durationUs = result.stdout
    .split(/\r?\n/)
    .map((line) => line.match(/^out_time_(?:us|ms)=(\d+)$/)?.[1])
    .filter(Boolean)
    .reduce((maximum, value) => Math.max(maximum, Number(value)), 0);
  if (!Number.isFinite(durationUs) || durationUs <= 0) {
    throw new Error(`FFmpeg did not report a video duration for ${label}`);
  }
  return Math.ceil(durationUs / 1000);
}

function samplingFps(durationMs) {
  if (!durationMs) return appearance.fps;
  return Math.max(
    0.01,
    Math.min(appearance.fps, (appearance.maxFrames * 1000) / durationMs),
  );
}

function exportedTiming(durationMs, frameCount) {
  if (!Number.isInteger(frameCount) || frameCount <= 0) {
    throw new Error("P4 H.264 stream contains no frames");
  }
  const frameDurationMs = Math.max(1, Math.ceil(durationMs / frameCount));
  return {
    fps: Math.max(1, Math.min(appearance.fps, Math.round(1000 / frameDurationMs))),
    frameDurationMs,
  };
}

function computePackId(payloadAssets, manifestIdentity) {
  const digest = createHash("sha256");
  digest.update(Buffer.from("pet-manager-p4-pack-v1\0", "utf8"));
  digest.update(u64le(manifestIdentity.length));
  digest.update(manifestIdentity);
  for (const asset of [...payloadAssets].sort((left, right) => (
    left.devicePath < right.devicePath ? -1 : left.devicePath > right.devicePath ? 1 : 0
  ))) {
    const pathBytes = Buffer.from(asset.devicePath, "utf8");
    const bytes = readFileSync(asset.sourcePath);
    digest.update(u64le(pathBytes.length));
    digest.update(pathBytes);
    digest.update(u64le(bytes.length));
    digest.update(bytes);
  }
  return digest.digest("hex");
}

function copyValidCachedVideo(devicePath, outputPath, maxFrames) {
  const cached = join(profileRoot, devicePath);
  if (!existsSync(cached)) return null;
  try {
    const bytes = readFileSync(cached);
    assertNotGitLfsPointer(bytes, cached);
    const parsed = parseH264Stream(bytes);
    if (parsed.frames === 0 || parsed.frames > maxFrames) return null;
    mkdirSync(dirname(outputPath), { recursive: true });
    copyFileSync(cached, outputPath);
    return parsed;
  } catch (error) {
    if (String(error?.message || error).includes("Git LFS 指针")) throw error;
    return null;
  }
}

function copyValidCachedAudio(devicePath, outputPath, sampleRate) {
  const cached = join(profileRoot, devicePath);
  if (!existsSync(cached)) return false;
  try {
    const bytes = readFileSync(cached);
    assertNotGitLfsPointer(bytes, cached);
    if (!wavMatchesDeviceContract(bytes, sampleRate)) return false;
    mkdirSync(dirname(outputPath), { recursive: true });
    copyFileSync(cached, outputPath);
    return true;
  } catch (error) {
    if (String(error?.message || error).includes("Git LFS 指针")) throw error;
    return false;
  }
}

function replaceProfileRoot(stagingRoot) {
  const backup = join(readyBase, `.replace-${process.pid}-${Date.now()}`);
  if (existsSync(profileRoot)) renameSync(profileRoot, backup);
  try {
    renameSync(stagingRoot, profileRoot);
  } catch (error) {
    if (existsSync(backup)) renameSync(backup, profileRoot);
    throw error;
  }
  if (existsSync(backup)) rmSync(backup, { recursive: true, force: true });
}

export function prepareBuiltInP4Ready({ ffmpegPath = "", allowTranscode = true } = {}) {
  const videos = readdirSync(clipsDir)
    .filter((name) => extname(name).toLowerCase() === ".mp4")
    .sort();
  if (videos.length === 0) throw new Error(`no built-in MP4 clips found under ${clipsDir}`);

  mkdirSync(readyBase, { recursive: true });
  const cachedSourceDurations = new Map();
  const cachedAudioHashes = new Map();
  try {
    const cachedMetadata = JSON.parse(readFileSync(join(profileRoot, metadataName), "utf8"));
    for (const source of cachedMetadata.sourceAssets || []) {
      if (source.videoSha256 && Number.isFinite(source.durationMs) && source.durationMs > 0) {
        cachedSourceDurations.set(source.videoSha256, source.durationMs);
      }
      if (source.family && source.audioSha256) {
        cachedAudioHashes.set(source.family, source.audioSha256);
      }
    }
  } catch {
    // A missing or stale cache is handled by the normal transcode path below.
  }
  const stagingRoot = mkdtempSync(join(readyBase, ".prepare-"));
  const payloadAssets = [];
  const families = [];
  const sourceAssets = [];
  try {
    for (const videoName of videos) {
      const family = basename(videoName, extname(videoName));
      const videoSource = join(clipsDir, videoName);
      const videoBytes = readFileSync(videoSource);
      const videoHash = sha256(videoBytes);
      const videoDevicePath = `p4/families/sha256-${videoHash.slice(0, 24)}.h264`;
      const videoOutput = join(stagingRoot, videoDevicePath);
      const cachedDurationMs = cachedSourceDurations.get(videoHash);
      const durationMs = ffmpegPath
        ? probeVideoDurationMs(ffmpegPath, videoSource, videoName)
        : cachedDurationMs;
      if (!Number.isFinite(durationMs) || durationMs <= 0) {
        throw new Error(`P4 ready timing metadata is missing or stale: ${videoName}`);
      }
      let stream = copyValidCachedVideo(videoDevicePath, videoOutput, appearance.maxFrames);
      if (!stream) {
        if (!allowTranscode) {
          throw new Error(`P4 ready video is missing or stale: ${videoName}`);
        }
        mkdirSync(dirname(videoOutput), { recursive: true });
        runFfmpeg(
          ffmpegPath,
          [
            "-y",
            "-v",
            "error",
            "-i",
            videoSource,
            "-vf",
            `fps=${samplingFps(durationMs).toFixed(6)},scale=${appearance.width}:${appearance.height}:force_original_aspect_ratio=decrease,pad=${appearance.width}:${appearance.height}:(ow-iw)/2:(oh-ih)/2:black`,
            "-frames:v",
            String(appearance.maxFrames),
            "-an",
            "-c:v",
            "libx264",
            "-preset",
            "medium",
            "-tune",
            "zerolatency",
            "-profile:v",
            "baseline",
            "-level:v",
            "3.0",
            "-crf",
            String(appearance.h264Crf),
            "-pix_fmt",
            "yuv420p",
            "-x264-params",
            `cabac=0:bframes=0:ref=1:weightp=0:scenecut=0:keyint=${appearance.maxFrames}:min-keyint=${appearance.maxFrames}:repeat-headers=1:aud=1:threads=1:sliced-threads=0`,
            "-f",
            "h264",
            videoOutput,
          ],
          videoName,
        );
        const compatible = rewriteH264Sps(
          readFileSync(videoOutput),
          appearance.width,
          appearance.height,
        );
        writeFileSync(videoOutput, compatible);
        stream = parseH264Stream(compatible);
      }
      const timing = exportedTiming(durationMs, stream.frames);
      const familyEntry = {
        family,
        path: videoDevicePath,
        frames: stream.frames,
        streamBytes: stream.streamBytes,
        fps: timing.fps,
        frameDurationMs: timing.frameDurationMs,
        durationMs,
      };
      payloadAssets.push({ devicePath: videoDevicePath, sourcePath: videoOutput });
      sourceAssets.push({ family, videoSha256: videoHash, durationMs });

      const audioSource = join(clipsDir, `${family}.wav`);
      if (existsSync(audioSource)) {
        const audioBytes = readFileSync(audioSource);
        const audioHash = sha256(audioBytes);
        const audioDevicePath = `p4/audio/${family}.wav`;
        const audioOutput = join(stagingRoot, audioDevicePath);
        const copiedCachedAudio = cachedAudioHashes.get(family) === audioHash
          && copyValidCachedAudio(
            audioDevicePath,
            audioOutput,
            appearance.audioSampleRate,
          );
        if (!copiedCachedAudio) {
          mkdirSync(dirname(audioOutput), { recursive: true });
          if (wavMatchesDeviceContract(audioBytes, appearance.audioSampleRate)) {
            copyFileSync(audioSource, audioOutput);
          } else {
            if (!allowTranscode) {
              throw new Error(`P4 ready audio is missing or stale: ${family}.wav`);
            }
            runFfmpeg(
              ffmpegPath,
              [
                "-y",
                "-v",
                "error",
                "-i",
                audioSource,
                "-map",
                "0:a:0",
                "-vn",
                "-ac",
                "1",
                "-ar",
                String(appearance.audioSampleRate),
                "-c:a",
                "pcm_s16le",
                "-f",
                "wav",
                audioOutput,
              ],
              `${family}.wav`,
            );
          }
        }
        if (!wavMatchesDeviceContract(readFileSync(audioOutput), appearance.audioSampleRate)) {
          throw new Error(`normalized P4 audio is invalid: ${family}.wav`);
        }
        familyEntry.audioPath = audioDevicePath;
        payloadAssets.push({ devicePath: audioDevicePath, sourcePath: audioOutput });
        sourceAssets[sourceAssets.length - 1].audioSha256 = audioHash;
      }
      families.push(familyEntry);
    }

    const manifest = {
      format: "p4-h264-v1",
      packId: "",
      codec: "h264",
      container: "annex-b",
      width: appearance.width,
      height: appearance.height,
      fps: appearance.fps,
      families,
    };
    const identity = Buffer.from(stableJson(manifest), "utf8");
    manifest.packId = computePackId(payloadAssets, identity);
    const expectedPackId = String(appearance.packId || "").trim();
    if (!/^[0-9a-f]{64}$/.test(expectedPackId)) {
      throw new Error("factory-config appearance.packId must be 64 lowercase hexadecimal characters");
    }
    if (manifest.packId !== expectedPackId) {
      throw new Error(
        `P4 ready pack differs from factory-config pin: generated ${manifest.packId}, expected ${expectedPackId}`,
      );
    }
    const manifestPath = join(stagingRoot, "p4", "manifest.json");
    mkdirSync(dirname(manifestPath), { recursive: true });
    writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
    writeFileSync(
      join(stagingRoot, metadataName),
      `${JSON.stringify({
        schemaVersion: 4,
        profile,
        packId: manifest.packId,
        sourceAssets,
      }, null, 2)}\n`,
    );

    const parsed = JSON.parse(readFileSync(manifestPath, "utf8"));
    const recordedPackId = parsed.packId;
    parsed.packId = "";
    const verifiedPackId = computePackId(payloadAssets, Buffer.from(stableJson(parsed), "utf8"));
    if (verifiedPackId !== recordedPackId) {
      throw new Error(`P4 ready pack verification failed: ${recordedPackId} != ${verifiedPackId}`);
    }
    replaceProfileRoot(stagingRoot);
    const totalBytes = payloadAssets.reduce(
      (sum, asset) => sum + readFileSync(join(profileRoot, asset.devicePath)).length,
      readFileSync(join(profileRoot, "p4", "manifest.json")).length,
    );
    return {
      profile,
      packId: recordedPackId,
      families: families.length,
      files: payloadAssets.length + 1,
      bytes: totalBytes,
      root: profileRoot,
    };
  } catch (error) {
    if (existsSync(stagingRoot)) rmSync(stagingRoot, { recursive: true, force: true });
    throw error;
  }
}

function cliFfmpegPath(argv) {
  const inline = argv.find((value) => value.startsWith("--ffmpeg="));
  const index = argv.indexOf("--ffmpeg");
  if (inline) return resolve(inline.slice("--ffmpeg=".length));
  if (index >= 0 && argv[index + 1]) return resolve(argv[index + 1]);
  const bundled = join(
    repositoryRoot,
    "pc",
    "src-tauri",
    "generated-runtime",
    process.platform === "win32" ? "ffmpeg.exe" : "ffmpeg",
  );
  return existsSync(bundled) ? bundled : "ffmpeg";
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const validateOnly = process.argv.includes("--validate-only");
  const summary = prepareBuiltInP4Ready({
    ffmpegPath: validateOnly ? "" : cliFfmpegPath(process.argv.slice(2)),
    allowTranscode: !validateOnly,
  });
  console.log(
    `P4 ready assets: profile=${summary.profile} packId=${summary.packId} files=${summary.files} bytes=${summary.bytes}`,
  );
}
