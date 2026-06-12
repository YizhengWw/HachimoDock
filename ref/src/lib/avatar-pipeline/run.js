/**
 * [Input] image + provider config + onProgress callback + AbortSignal.
 * [Output] orchestrates Ark Responses thinking model + low-resolution N family video tasks in parallel;
 *          resolves with per-family results.
 * [Pos] orchestrator node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header.
 */

import { FAMILIES } from "./families.js";
import { buildImagePayload, bytesToDataUrl, uint8ToBase64 } from "./image.js";
import { processImageForPipeline } from "./image-processing.js";
import { resolveGenerationSpeedConfig } from "./pipeline-defaults.js";
import { buildDryRunResponse, normalizePromptResponse } from "./prompts.js";
import { callThinkingModel, DEFAULT_THINKING_MODEL } from "./thinking-model.js";
import { runVolcanoFamily } from "./providers/volcano.js";
import { runKlingFamily } from "./providers/kling.js";
import { runCustomFamily } from "./providers/custom.js";

function isVolcanoVideoModelName(model) {
  const normalized = String(model || "").trim().toLowerCase();
  return normalized.includes("seedance") || normalized.includes("video");
}

export function resolveThinkingModelName(providerConfig = {}) {
  const model = String(providerConfig.model || "").trim();
  const thinkingModel = String(providerConfig.thinkingModel || "").trim();
  if (providerConfig.provider === "volcengine") {
    if (!thinkingModel || thinkingModel === model || isVolcanoVideoModelName(thinkingModel)) {
      return DEFAULT_THINKING_MODEL;
    }
  }
  return thinkingModel || model || DEFAULT_THINKING_MODEL;
}

/**
 * @typedef {{
 *   stage: 'processing' | 'thinking' | 'generating' | 'saving' | 'done',
 *   completed: number,
 *   total: number,
 *   families: Record<string, { status: 'pending'|'submitting'|'polling'|'downloading'|'done'|'failed', error?: string, taskId?: string }>,
 *   message?: string,
 * }} PipelineProgress
 */

function newFamilyState(families) {
  const out = {};
  for (const f of families) out[f.family] = { status: "pending" };
  return out;
}

function setStatus(progress, family, patch) {
  progress.families[family] = { ...progress.families[family], ...patch };
}

/**
 * Run the full pipeline.
 *
 * @param {object} args
 * @param {File} args.imageFile         user-uploaded image File
 * @param {string} [args.appearanceName]
 * @param {string} [args.personality]
 * @param {{
 *   provider: 'volcengine'|'kling'|'custom',
 *   apiKey?: string,
 *   accessKey?: string,
 *   secretKey?: string,
 *   baseUrl?: string,
 *   model?: string,
 *   thinkingModel?: string,              Ark prompt-generation model name
 *   generateAudio?: boolean,
 *   watermark?: boolean,
 *   advanced?: object,
 * }} args.providerConfig
 * @param {AbortSignal} [args.signal]
 * @param {(progress: PipelineProgress) => void} [args.onProgress]
 * @param {(info: {
 *   persona: object,
 *   manifest: object,
 *   imagePayload: { bytes: Uint8Array, mime: string, filename: string },
 *   family: { family: string, prompt: string, ok: true, videoBytes: Uint8Array, videoUrl?: string, taskId?: string },
 * }) => (void | Promise<void>)} [args.onFamilyDone]
 *   Called as soon as each family finishes successfully (before the overall
 *   promise resolves). Used to persist partial results so a stuck final family
 *   doesn't keep earlier successes trapped in memory.
 * @param {boolean} [args.dryRun]
 * @returns {Promise<{
 *   persona: object,
 *   manifest: object,
 *   imagePayload: { bytes: Uint8Array, mime: string, filename: string },
 *   families: Array<{ family: string, prompt: string, ok: boolean, videoBytes?: Uint8Array, error?: string, taskId?: string, videoUrl?: string }>,
 * }>}
 */
export async function runAvatarPipeline({
  imageFile,
  appearanceName = "",
  personality = "",
  providerConfig,
  signal,
  onProgress,
  onFamilyDone,
  dryRun = false,
  skipProcessing = false,
}) {
  if (!imageFile) throw new Error("imageFile is required");
  if (!providerConfig) throw new Error("providerConfig is required");
  const runtimeConfig = resolveGenerationSpeedConfig(providerConfig);
  const effectiveProviderConfig = runtimeConfig.providerConfig;

  const total = FAMILIES.length;
  const progress = {
    stage: "processing",
    completed: 0,
    total,
    families: newFamilyState(FAMILIES),
    message: "正在处理图片（去除背景）…",
  };
  const emit = () => onProgress?.({ ...progress, families: { ...progress.families } });
  emit();

  const originalImagePayload = await buildImagePayload(imageFile);

  // Image processing: background removal + black bg + downscale
  let imagePayload;
  if (skipProcessing || dryRun) {
    imagePayload = originalImagePayload;
  } else {
    const processed = await processImageForPipeline(imageFile, {
      maxDimension: runtimeConfig.imageMaxDimension,
      signal,
      onProgress: (stage) => {
        const labels = {
          removing_bg: "正在去除背景…",
          compositing: "正在合成黑色背景…",
          done: "图片处理完成",
        };
        progress.message = labels[stage] || "处理中…";
        emit();
      },
    });
    const processedMime = "image/png";
    const processedBase64 = uint8ToBase64(processed.processedBytes);
    imagePayload = {
      bytes: processed.processedBytes,
      mime: processedMime,
      base64: processedBase64,
      dataUrl: `data:${processedMime};base64,${processedBase64}`,
      filename: originalImagePayload.filename,
    };
  }

  // 1) Thinking model
  progress.stage = "thinking";
  progress.message = "正在分析图片并生成 prompt…";
  emit();

  let thinkingResult;
  if (dryRun) {
    thinkingResult = {
      persona: buildDryRunResponse({ families: FAMILIES, sourceImageName: imageFile.name }).persona,
      prompts: buildDryRunResponse({ families: FAMILIES, sourceImageName: imageFile.name }).prompts,
      raw: { dry_run: true },
    };
  } else {
    thinkingResult = await callThinkingModel({
      thinking: {
        apiKey: effectiveProviderConfig.apiKey || effectiveProviderConfig.accessKey || "",
        baseUrl: effectiveProviderConfig.baseUrl,
        model: resolveThinkingModelName(effectiveProviderConfig),
        apiUrlOverride: effectiveProviderConfig.thinkingApiUrl,
      },
      image: imagePayload,
      appearanceName,
      personality,
      signal,
    });
  }

  const manifest = normalizePromptResponse({
    response: thinkingResult,
    families: FAMILIES,
  });

  // 2) Submit + poll + download per family with bounded concurrency.
  // Providers (Volcano/Kling) enforce account-level concurrency on their side,
  // so hammering 10 submissions just parks most of them in a server queue.
  // Running ~3 at a time keeps the UI progress honest and avoids wasting
  // polling traffic on jobs that haven't started yet.
  const CONCURRENCY = 3;
  progress.stage = "generating";
  progress.message = `正在生成 ${manifest.entries.length} 个动画 (${runtimeConfig.fastGeneration ? "快速低清" : "标准"} / 并发 ${CONCURRENCY})…`;
  emit();

  async function runOne(entry) {
    setStatus(progress, entry.family, { status: "submitting" });
    emit();

    try {
      const familyOnStage = ({ stage, detail }) => {
        setStatus(progress, entry.family, { status: stage, taskId: detail?.taskId });
        emit();
      };

      let result;
      if (dryRun) {
        // Synthesize a tiny "video" payload so downstream code can write a placeholder MP4.
        // These are the leading bytes of an MP4 ISO-BMFF `ftyp` box; safe inert filler — keeps the rest of the pipeline honest in tests.
        await new Promise((r) => setTimeout(r, 60));
        result = {
          taskId: `dry-run-${entry.family}`,
          videoBytes: new Uint8Array([0x00, 0x00, 0x00, 0x20, 0x66, 0x74, 0x79, 0x70]),
          videoUrl: "dry-run://placeholder",
          raw: { dry_run: true },
        };
      } else if (effectiveProviderConfig.provider === "kling") {
        result = await runKlingFamily({
          config: {
            accessKey: effectiveProviderConfig.accessKey || effectiveProviderConfig.apiKey,
            secretKey: effectiveProviderConfig.secretKey,
            baseUrl: effectiveProviderConfig.baseUrl,
            model: effectiveProviderConfig.model,
            mode: effectiveProviderConfig.mode,
            duration: effectiveProviderConfig.duration,
          },
          prompt: entry.prompt,
          imageBase64: imagePayload.base64,
          signal,
          onStage: familyOnStage,
        });
      } else if (effectiveProviderConfig.provider === "custom") {
        result = await runCustomFamily({
          config: {
            apiKey: effectiveProviderConfig.apiKey,
            baseUrl: effectiveProviderConfig.baseUrl,
            model: effectiveProviderConfig.model,
            advanced: effectiveProviderConfig.advanced,
          },
          prompt: entry.prompt,
          imageDataUrl: imagePayload.dataUrl,
          signal,
          onStage: familyOnStage,
        });
      } else {
        result = await runVolcanoFamily({
          config: {
            apiKey: effectiveProviderConfig.apiKey,
            baseUrl: effectiveProviderConfig.baseUrl,
            model: effectiveProviderConfig.model,
            duration: effectiveProviderConfig.duration,
            ratio: effectiveProviderConfig.ratio,
            resolution: effectiveProviderConfig.resolution,
            generateAudio: effectiveProviderConfig.generateAudio,
            watermark: effectiveProviderConfig.watermark,
          },
          prompt: entry.prompt,
          imageDataUrl: imagePayload.dataUrl,
          signal,
          onStage: familyOnStage,
        });
      }

      setStatus(progress, entry.family, { status: "done", taskId: result.taskId });
      progress.completed += 1;
      emit();

      const familyResult = {
        family: entry.family,
        prompt: entry.prompt,
        ok: true,
        videoBytes: result.videoBytes,
        videoUrl: result.videoUrl,
        taskId: result.taskId,
      };
      // Fire-and-await the incremental-save hook so wizard can persist this
      // family immediately instead of waiting for the slowest sibling.
      if (onFamilyDone) {
        try {
          await onFamilyDone({
            persona: manifest.persona,
            manifest,
            imagePayload: originalImagePayload,
            family: familyResult,
          });
        } catch (hookErr) {
          console.error(`[avatar-pipeline] onFamilyDone hook failed for ${entry.family}:`, hookErr);
        }
      }
      return familyResult;
    } catch (err) {
      const message = err?.message || String(err);
      // Log the full error so the UI's condensed `message` (e.g. bare "Type error")
      // can be traced to the actual network call / stack that threw.
      console.error(`[avatar-pipeline] family="${entry.family}" failed:`, err);
      setStatus(progress, entry.family, { status: "failed", error: message });
      progress.completed += 1;
      emit();
      return {
        family: entry.family,
        prompt: entry.prompt,
        ok: false,
        error: message,
      };
    }
  }

  const familiesResult = new Array(manifest.entries.length);
  let cursor = 0;
  async function worker() {
    while (true) {
      if (signal?.aborted) return;
      const idx = cursor++;
      if (idx >= manifest.entries.length) return;
      familiesResult[idx] = await runOne(manifest.entries[idx]);
    }
  }
  const workerCount = Math.min(CONCURRENCY, manifest.entries.length);
  await Promise.all(Array.from({ length: workerCount }, () => worker()));

  // Abort propagation: if user cancelled, raise to caller (so we don't accidentally save).
  if (signal?.aborted) throw new DOMException("aborted", "AbortError");

  // Allow partial success: caller decides if there's at least one to save.
  progress.stage = "saving";
  progress.message = "正在写入本地资源…";
  emit();

  return {
    persona: manifest.persona,
    manifest,
    imagePayload: {
      bytes: originalImagePayload.bytes,
      mime: originalImagePayload.mime,
      filename: originalImagePayload.filename,
    },
    families: familiesResult,
  };
}

/**
 * Re-run a single family (used from AppearanceDetail when one family failed previously).
 *
 * @param {object} args
 * @param {Uint8Array} args.imageBytes
 * @param {string} args.imageMime
 * @param {string} args.prompt              the prompt previously generated for this family
 * @param {object} args.providerConfig      same shape as runAvatarPipeline().providerConfig
 * @param {AbortSignal} [args.signal]
 * @param {(event: { stage: string, detail?: any }) => void} [args.onStage]
 */
export async function runSingleFamilyRetry({
  imageBytes,
  imageMime,
  prompt,
  providerConfig,
  signal,
  onStage,
}) {
  if (!providerConfig) throw new Error("providerConfig is required");
  const runtimeConfig = resolveGenerationSpeedConfig(providerConfig);
  const effectiveProviderConfig = runtimeConfig.providerConfig;
  const imageDataUrl = bytesToDataUrl(imageBytes, imageMime || "image/png");
  const imageBase64 = uint8ToBase64(imageBytes);

  if (effectiveProviderConfig.provider === "kling") {
    return runKlingFamily({
      config: {
        accessKey: effectiveProviderConfig.accessKey || effectiveProviderConfig.apiKey,
        secretKey: effectiveProviderConfig.secretKey,
        baseUrl: effectiveProviderConfig.baseUrl,
        model: effectiveProviderConfig.model,
        mode: effectiveProviderConfig.mode,
        duration: effectiveProviderConfig.duration,
      },
      prompt,
      imageBase64,
      signal,
      onStage,
    });
  }
  if (effectiveProviderConfig.provider === "custom") {
    return runCustomFamily({
      config: {
        apiKey: effectiveProviderConfig.apiKey,
        baseUrl: effectiveProviderConfig.baseUrl,
        model: effectiveProviderConfig.model,
        advanced: effectiveProviderConfig.advanced,
      },
      prompt,
      imageDataUrl,
      signal,
      onStage,
    });
  }
  return runVolcanoFamily({
    config: {
      apiKey: effectiveProviderConfig.apiKey,
      baseUrl: effectiveProviderConfig.baseUrl,
      model: effectiveProviderConfig.model,
      duration: effectiveProviderConfig.duration,
      ratio: effectiveProviderConfig.ratio,
      resolution: effectiveProviderConfig.resolution,
      generateAudio: effectiveProviderConfig.generateAudio,
      watermark: effectiveProviderConfig.watermark,
    },
    prompt,
    imageDataUrl,
    signal,
    onStage,
  });
}
