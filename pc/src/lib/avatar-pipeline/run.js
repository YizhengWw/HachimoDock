/**
 * [Input] image + optional visual style + provider config + reusable preview checkpoint + onProgress callback + AbortSignal.
 * [Output] orchestrates Ark Seedream background editing + Responses prompt generation + low-resolution
 *          bounded parallel video tasks with shared-reference rejection stopping unsubmitted actions,
 *          or a user-prompted single-family replacement; completed results remain available.
 *          Seedream input/output moderation stops preprocessing before prompt/video calls.
 * [Pos] orchestrator node in pc/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header.
 */

import { CUSTOM_GENERATION_FAMILIES, FAMILIES } from "./families.js";
import { buildImagePayload, bytesToDataUrl, uint8ToBase64 } from "./image.js";
import { processImageForPipeline } from "./image-processing.js";
import { resolveGenerationSpeedConfig, THINKING_IMAGE_DIMENSION } from "./pipeline-defaults.js";
import { requireAvailableVideoModel } from "./video-models.js";
import { buildDryRunResponse, normalizeFamilies, normalizePromptResponse, parseModelJson } from "./prompts.js";
import { callThinkingModel, DEFAULT_THINKING_MODEL } from "./thinking-model.js";
import { getVolcanoInputImageRejection, runVolcanoFamily } from "./providers/volcano.js";
import { DEFAULT_VOLCANO_IMAGE_MODEL } from "./providers/volcano-image.js";
import { runKlingFamily } from "./providers/kling.js";
import { runCustomFamily } from "./providers/custom.js";
import { assertResumableAction, serialQueue } from "./session.js";
import { inspectReference, inspectVideo, checkpointedReview, isRecoverableQueryError } from "./quality.js";
import {ensureActionStartFrame} from './action-start-frame.js';

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

function resolveFamilyDefinition(family) {
  if (family && typeof family === "object") return normalizeFamilies([family])[0];
  const familyId = String(family || "").trim();
  const known = FAMILIES.find((item) => item.family === familyId);
  if (!known) throw new Error(`unknown family: ${familyId || "(empty)"}`);
  return normalizeFamilies([known])[0];
}

export function resolveArkBackgroundConfig(providerConfig = {}) {
  const independentConfig = providerConfig.imageEdit || {};
  const selectedProviderKey = providerConfig.provider === "volcengine" ? providerConfig.apiKey : "";
  const apiKey = String(independentConfig.apiKey || selectedProviderKey || "").trim();
  if (!apiKey) return null;
  return {
    apiKey,
    baseUrl: independentConfig.baseUrl || providerConfig.baseUrl,
    model: independentConfig.model || providerConfig.imageModel || DEFAULT_VOLCANO_IMAGE_MODEL,
  };
}

export function buildSingleFamilyManifest({ family, prompt }) {
  const familyDef = resolveFamilyDefinition(family);
  const normalizedPrompt = String(prompt || "").trim();
  if (!normalizedPrompt) throw new Error("单状态重生成需要填写 prompt");
  return {
    manifest_version: 1,
    mode: "single_family_video",
    persona: {},
    entries: [
      {
        ...familyDef,
        prompt: normalizedPrompt,
        variation_notes: "user supplied single-state prompt",
      },
    ],
  };
}

async function buildRuntimeImagePayload({
  imageFile,
  runtimeConfig,
  signal,
  onProgress,
  skipProcessing,
  dryRun,
}) {
  const originalImagePayload = await buildImagePayload(imageFile);
  if (skipProcessing || dryRun) return originalImagePayload;

  const processed = await processImageForPipeline(imageFile, {
    maxDimension: runtimeConfig.imageMaxDimension,
    cloudBackground: resolveArkBackgroundConfig(runtimeConfig.providerConfig),
    signal,
    onProgress,
  });
  const processedMime = "image/png";
  const processedBase64 = uint8ToBase64(processed.processedBytes);
  return {
    bytes: processed.processedBytes,
    mime: processedMime,
    base64: processedBase64,
    dataUrl: `data:${processedMime};base64,${processedBase64}`,
    filename: originalImagePayload.filename,
  };
}

async function runProviderFamily({
  providerConfig,
  prompt,
  imagePayload,
  signal,
  onStage,
}) {
  if (providerConfig.provider === "kling") {
    return runKlingFamily({
      config: {
        accessKey: providerConfig.accessKey || providerConfig.apiKey,
        secretKey: providerConfig.secretKey,
        baseUrl: providerConfig.baseUrl,
        model: providerConfig.model,
        mode: providerConfig.mode,
        duration: providerConfig.duration,
      },
      prompt,
      imageBase64: imagePayload.base64,
      signal,
      onStage,
    });
  }
  if (providerConfig.provider === "custom") {
    return runCustomFamily({
      config: {
        apiKey: providerConfig.apiKey,
        baseUrl: providerConfig.baseUrl,
        model: providerConfig.model,
        advanced: providerConfig.advanced,
      },
      prompt,
      imageDataUrl: imagePayload.dataUrl,
      signal,
      onStage,
    });
  }
  return runVolcanoFamily({
    config: {
      apiKey: providerConfig.apiKey,
      baseUrl: providerConfig.baseUrl,
      model: providerConfig.model,
      duration: providerConfig.duration,
      ratio: providerConfig.ratio,
      resolution: providerConfig.resolution,
      generateAudio: providerConfig.generateAudio,
      watermark: providerConfig.watermark,
      cameraFixed: providerConfig.cameraFixed,
      seed: providerConfig.seed,
    },
    prompt,
    imageDataUrl: imagePayload.dataUrl,
    signal,
    onStage,
  });
}

export async function runSingleFamilyVideo({
  imageFile,
  family,
  prompt,
  providerConfig,
  signal,
  onProgress,
  dryRun = false,
  skipProcessing = false,
}) {
  if (!imageFile) throw new Error("imageFile is required");
  if (!providerConfig) throw new Error("providerConfig is required");
  const manifest = buildSingleFamilyManifest({ family, prompt });
  if (!dryRun) await requireAvailableVideoModel(providerConfig, signal);
  const entry = manifest.entries[0];
  const runtimeConfig = resolveGenerationSpeedConfig(providerConfig);
  const effectiveProviderConfig = runtimeConfig.providerConfig;

  const emit = (patch) => {
    onProgress?.({
      family: entry.family,
      stage: "processing",
      message: "正在使用方舟图片模型处理背景并生成参考帧…",
      ...patch,
    });
  };

  emit({ stage: "processing" });
  const imagePayload = await buildRuntimeImagePayload({
    imageFile,
    runtimeConfig,
    signal,
    skipProcessing,
    dryRun,
    onProgress: (stage, progress) => {
      const labels = {
        cloud_editing: "正在使用方舟图片模型替换背景…",
        cloud_fallback: "方舟背景处理不可用，正在使用原图继续…",
        compositing: "正在生成 4:3 黑底参考图…",
        done: "图片处理完成",
      };
      emit({ stage: "processing", message: labels[stage] || "处理中…", progress });
    },
  });

  emit({ stage: "generating", status: "submitting", message: `正在生成 ${entry.family} 状态视频…` });

  let result;
  if (dryRun) {
    await new Promise((resolve) => setTimeout(resolve, 60));
    result = {
      taskId: `dry-run-${entry.family}`,
      videoBytes: new Uint8Array([0x00, 0x00, 0x00, 0x20, 0x66, 0x74, 0x79, 0x70]),
      videoUrl: "dry-run://placeholder",
      raw: { dry_run: true },
    };
  } else {
    result = await runProviderFamily({
      providerConfig: effectiveProviderConfig,
      prompt: entry.prompt,
      imagePayload,
      signal,
      onStage: ({ stage, detail }) => {
        const stageLabels = {
          submitting: "正在提交视频生成任务…",
          polling: "正在等待视频生成完成…",
          downloading: "正在下载生成结果…",
          done: "单状态视频生成完成",
        };
        emit({
          stage: "generating",
          status: stage,
          taskId: detail?.taskId,
          message: stageLabels[stage] || "正在生成视频…",
        });
      },
    });
  }

  emit({ stage: "done", status: "done", taskId: result.taskId, message: "单状态视频生成完成" });
  return {
    persona: {},
    manifest,
    imagePayload,
    family: {
      family: entry.family,
      prompt: entry.prompt,
      ok: true,
      videoBytes: result.videoBytes,
      taskId: result.taskId,
      videoUrl: result.videoUrl,
    },
  };
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
 *   imageEdit?: { apiKey?: string, baseUrl?: string, model?: string }, independent Ark Seedream image-edit config
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
  session = null,
  checkpoint,
  confirmReference,
  confirmTrial,
  familyIds,
  automatic = false,
  retryReview = false,
  styleDescription = '',
  distinctStartFrames = false,
}) {
  if (!imageFile) throw new Error("imageFile is required");
  if (!providerConfig) throw new Error("providerConfig is required");
  const runtimeConfig = resolveGenerationSpeedConfig(providerConfig);
  const effectiveProviderConfig = runtimeConfig.providerConfig;
  const generationFamilies = familyIds ? familyIds.map(resolveFamilyDefinition) : CUSTOM_GENERATION_FAMILIES;
  if (!generationFamilies.length || new Set(generationFamilies.map(f => f.family)).size !== generationFamilies.length) throw new Error("动作列表为空或重复");
  const commitQueue = serialQueue();
  let persistenceError = null;
  const commit = async (patch) => {
    if (persistenceError) throw persistenceError;
    if (session) Object.assign(session, patch);
    try { await commitQueue(() => checkpoint?.(session)); }
    catch (error) { persistenceError = error; throw error; }
  };
  const saveResult = serialQueue();
  const thinking = { apiKey: effectiveProviderConfig.apiKey || effectiveProviderConfig.accessKey || "",
    baseUrl: effectiveProviderConfig.baseUrl, model: resolveThinkingModelName(effectiveProviderConfig), apiUrlOverride: effectiveProviderConfig.thinkingApiUrl };
  let inputInspection;
  if (automatic && !dryRun) {
    if(!session)throw Error("自动生成需要可恢复的任务记录");
    if(effectiveProviderConfig.provider!=="volcengine")throw Error("当前一键自动检查支持火山引擎；其他服务请在高级设置选择逐步确认模式");
    if(!thinking.apiKey||(!skipProcessing&&!resolveArkBackgroundConfig(effectiveProviderConfig)?.apiKey))throw Error("请先配置图片、分析和视频服务，尚未产生生成费用");
    if(/deepseek/i.test(thinking.model))throw Error("自动检查需要支持图片的分析模型，尚未产生生成费用");
    if(!session.referenceApproved)inputInspection=await inspectReference(imageFile,signal);
  }
  if (!dryRun) await requireAvailableVideoModel(providerConfig, signal);

  const total = generationFamilies.length;
  const progress = {
    stage: "processing",
    completed: 0,
    total,
    families: newFamilyState(generationFamilies),
    message: skipProcessing ? "正在准备原图，不调用背景处理模型…" : "正在使用方舟图片模型处理背景…",
  };
  const emit = () => onProgress?.({ ...progress, families: { ...progress.families } });
  emit();

  let originalImagePayload = session?.original;
  if(!originalImagePayload) {
    try {originalImagePayload=await buildImagePayload(imageFile);}
    catch(error) {
      // Legacy WebKit File may reference a deleted blob backing file. Approved
      // reference bytes are sufficient for remaining video work; never edit or
      // regenerate them, and never pretend the processed image is the original.
      if(!session?.referenceApproved || !session?.reference?.bytes?.byteLength)throw error;
      originalImagePayload=null;
      await commit({originalUnavailable:true});
    }
    if(originalImagePayload)await commit({original:originalImagePayload});
  }

  // Image processing: Ark background edit + local black-frame normalization.
  let imagePayload;
  if (session?.reference) {
    imagePayload = session.reference;
  } else if (skipProcessing || dryRun) {
    imagePayload = originalImagePayload;
  } else {
    if (session?.imageIntent) throw new Error("上次图片处理结果未知，不会自动再次调用付费服务；请核对后重新创建。");
    await commit({ imageIntent: true });
    const processed = await processImageForPipeline(imageFile, {
      styleDescription,
      maxDimension: runtimeConfig.imageMaxDimension,
      cloudBackground: resolveArkBackgroundConfig(effectiveProviderConfig),
      signal,
      onProgress: (stage, imageProgress) => {
        const labels = {
          cloud_editing: "正在使用方舟图片模型替换背景…",
          cloud_fallback: "方舟背景处理不可用，正在使用原图继续…",
          compositing: "正在生成 4:3 黑底参考图…",
          done: "图片处理完成",
        };
        progress.message = labels[stage] || "处理中…";
        progress.imageProgress = imageProgress;
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
      master: processed.masterImage,
    };
  }

  await commit({ reference: imagePayload });
  if (automatic && !dryRun && !session?.referenceApproved) {
    progress.stage='reference_review';progress.message="正在自动检查角色一致性、完整性与背景清理结果…";emit();
    if(!session?.referenceQuality){
      await checkpointedReview({owner:session,key:'referenceQuality',commit,retryReview,thinking,images:[inputInspection.dataUrl,imagePayload.dataUrl],kind:'reference',signal,styleDescription});
    }
    if(!session.referenceQuality.pass)throw Error(`参考图自动检查未通过：${session.referenceQuality.issues.join('；')}。原图与已有结果已保留，未继续提交视频。处理结果不合格不等于原图不支持；不会自动重复付费生成。`);
    await commit({referenceApproved:true});
  }
  if (confirmReference && !session?.referenceApproved) {
    await commit({ phase: "reference_review" });
    await confirmReference(imagePayload);
    await commit({ referenceApproved: true });
  }

  // 1) Thinking model
  progress.stage = "thinking";
  await commit({ phase: "thinking" });
  progress.message = "正在分析图片并生成 prompt…";
  emit();

  let thinkingResult;
  if (session?.manifest) {
    thinkingResult = { persona: session.manifest.persona, prompts: session.manifest.entries };
  } else if (dryRun) {
    const dryRunResponse = buildDryRunResponse({ families: generationFamilies, sourceImageName: imageFile.name });
    thinkingResult = {
      persona: dryRunResponse.persona,
      prompts: dryRunResponse.prompts,
      raw: { dry_run: true },
    };
  } else {
    if (retryReview && session?.thinkingResponse) {
      try {
        const raw=session.thinkingResponse;
        if(raw.truncated || ['incomplete','failed'].includes(raw.status))throw Error('incomplete');
        normalizePromptResponse({response:parseModelJson(raw.output_text),families:generationFamilies});
      } catch {
        // Only an explicit resume can repeat a completed-but-invalid prompt
        // response. An unknown submission (no response) remains blocked below.
        session.thinkingPreviousResponse=session.thinkingResponse;
        session.thinkingResponse=null;session.thinkingIntent=false;await commit({});
      }
    }
    if (session?.thinkingIntent && !session?.thinkingResponse) throw new Error("上次提示词生成结果未知，已停止自动重发；参考图已保留，请先核对服务端结果。");
    await commit({ thinkingIntent: true });
    // Vision analysis needs fewer pixels than the final video reference.
    const small = skipProcessing ? null : await processImageForPipeline(new Blob([imagePayload.bytes], { type: imagePayload.mime }), {
      maxDimension: THINKING_IMAGE_DIMENSION, allowOriginalFallback: true, signal,
    });
    thinkingResult = await callThinkingModel({
      thinking: {
        apiKey: effectiveProviderConfig.apiKey || effectiveProviderConfig.accessKey || "",
        baseUrl: effectiveProviderConfig.baseUrl,
        model: resolveThinkingModelName(effectiveProviderConfig),
        apiUrlOverride: effectiveProviderConfig.thinkingApiUrl,
      },
      image: small ? { ...imagePayload, dataUrl: bytesToDataUrl(small.processedBytes, "image/png") } : imagePayload,
      families: generationFamilies,
      appearanceName,
      personality,
      signal,
      cachedResponse: session?.thinkingResponse,
      onResponse: async thinkingResponse => commit({ thinkingResponse }),
    });
  }

  const manifest = normalizePromptResponse({
    response: thinkingResult,
    families: generationFamilies,
  });
  await commit({ manifest });

  // 2) Submit + poll + download per family with bounded concurrency.
  // Providers (Volcano/Kling) enforce account-level concurrency on their side,
  // so hammering 10 submissions just parks most of them in a server queue.
  // Running ~3 at a time keeps the UI progress honest and avoids wasting
  // polling traffic on jobs that haven't started yet.
  const CONCURRENCY = 3;
  progress.stage = "generating";
  progress.message = `正在生成 ${manifest.entries.length} 个动画 (${runtimeConfig.fastGeneration ? "快速低清" : "标准"} / 并发 ${CONCURRENCY})…`;
  emit();

  let sharedImageError = "";
  async function runOne(entry) {
    setStatus(progress, entry.family, { status: "submitting" });
    emit();

    try {
      const familyOnStage = ({ stage, detail }) => {
        setStatus(progress, entry.family, { status: stage, taskId: detail?.taskId });
        emit();
      };

      const prior = session?.actions?.[entry.family];
      assertResumableAction(prior, effectiveProviderConfig.provider);
      let actionImage=imagePayload;
      if(distinctStartFrames&&!dryRun&&!prior?.taskId&&!prior?.videoBytes&&effectiveProviderConfig.provider==='volcengine'){
        if(!session)throw Error('独立首帧生成需要可恢复的任务记录');
        session.actions ||= {};session.actions[entry.family] ||= {};
        setStatus(progress,entry.family,{status:'checking'});progress.message=`正在生成并检查 ${entry.family} 的独立起始姿势…`;emit();
        actionImage=await ensureActionStartFrame({owner:session.actions[entry.family],actions:session.actions,family:entry.family,reference:imagePayload,
          cloudBackground:resolveArkBackgroundConfig(effectiveProviderConfig),maxDimension:runtimeConfig.imageMaxDimension,thinking,signal,commit,retryReview});
      }
      let result;
      if (prior?.videoBytes) {
        result = prior;
      } else if (dryRun) {
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
        if (session) { session.actions ||= {}; session.actions[entry.family] = { intent: true }; await commit({}); }
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
        if (session) { session.actions ||= {}; session.actions[entry.family] = { intent: true }; await commit({}); }
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
        if (session && !prior?.taskId) { session.actions ||= {}; session.actions[entry.family] = { ...session.actions[entry.family],intent: true }; await commit({}); }
        result = await runVolcanoFamily({
          resumeTaskId: prior?.taskId,
          onSubmitted: async (taskId) => {
            if (session) { session.actions[entry.family] = { ...session.actions[entry.family],intent: true, taskId }; await commit({}); }
          },
          config: {
            apiKey: effectiveProviderConfig.apiKey,
            baseUrl: effectiveProviderConfig.baseUrl,
            model: effectiveProviderConfig.model,
            duration: effectiveProviderConfig.duration,
            ratio: effectiveProviderConfig.ratio,
            resolution: effectiveProviderConfig.resolution,
            generateAudio: effectiveProviderConfig.generateAudio,
            watermark: effectiveProviderConfig.watermark,
            cameraFixed: effectiveProviderConfig.cameraFixed,
            seed: effectiveProviderConfig.seed,
            firstFrameOnly:distinctStartFrames,
          },
          prompt: distinctStartFrames ? 'Start directly from this action-specific first-frame pose and continue the motion naturally. Do not reset to a shared neutral pose. '+entry.prompt : entry.prompt,
          imageDataUrl: actionImage.dataUrl,
          signal,
          onStage: familyOnStage,
        });
      }

      const familyResult = {
        family: entry.family,
        prompt: entry.prompt,
        ok: true,
        videoBytes: result.videoBytes,
        videoUrl: result.videoUrl,
        taskId: result.taskId,
      };
      if (session) {
        session.actions ||= {};
        // Do not persist signed CDN URLs; the task ID can retrieve a fresh URL.
        session.actions[entry.family] = { ...session.actions[entry.family], intent: true, taskId: result.taskId, videoBytes: result.videoBytes, deferred:false, failed:false };
        await commit({});
      }
      if(automatic&&!dryRun){
        setStatus(progress,entry.family,{status:'checking'});progress.message=`正在检查 ${entry.family} 的画面与格式…`;emit();
        const action=session.actions[entry.family];
        const checked=await inspectVideo(result.videoBytes,signal);
        if(entry.family===manifest.entries[0].family&&!session.trialApproved){
          progress.stage='trial_review';emit();
          action.inspection=checked;await commit({});
          if(!action.quality || retryReview){
            await checkpointedReview({owner:action,key:'quality',commit,retryReview,thinking,images:[imagePayload.dataUrl,...checked.frames],kind:'trial',signal});
          }
          if(!action.quality.pass)throw Error(`试片自动检查未通过：${action.quality.issues.join('；')}。视频已保留，未提交其余动作。`);
        }
        action.validated=true;await commit({});
      }
      // Fire-and-await the incremental-save hook so wizard can persist this
      // family immediately instead of waiting for the slowest sibling.
      if (onFamilyDone) {
        try {
          await saveResult(() => onFamilyDone({
            persona: manifest.persona,
            manifest,
            imagePayload,
            originalImagePayload,
            family: familyResult,
          }));
        } catch (hookErr) {
          persistenceError = new Error(`动画已生成但本地保存失败，已保留任务缓存；请恢复任务，不要重新生成。${hookErr?.message || hookErr}`);
          throw persistenceError;
        }
      }
      setStatus(progress, entry.family, { status: "done", taskId: result.taskId });
      progress.completed += 1;
      emit();
      return familyResult;
    } catch (err) {
      const message = err?.message || String(err);
      if (session && !persistenceError && !signal?.aborted && err?.name !== "AbortError") {
        session.actions ||= {};
        const action=session.actions[entry.family]||{};
        const deferred=!!action.taskId&&!action.videoBytes&&isRecoverableQueryError(err);
        session.actions[entry.family] = { ...action, failed: !deferred, deferred };
        await commit({});
      }
      if (!["kling", "custom"].includes(effectiveProviderConfig.provider)
        && getVolcanoInputImageRejection(err)) {
        sharedImageError ||= message;
        progress.referenceImageError = getVolcanoInputImageRejection(err).message;
        progress.message = "共用参考图未通过审核，已停止提交后续动作；等待已提交任务结束，保留成功结果。";
      }
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
  if ((confirmTrial || automatic) && !session?.trialApproved) {
    const first = await runOne(manifest.entries[0]);
    familiesResult[0] = first;
    cursor = 1;
    if (!first.ok) throw new Error(first.error);
    await commit({ phase: "trial_review" });
    if(confirmTrial)await confirmTrial(first, manifest.entries.length - 1);
    await commit({ trialApproved: true });
  }
  await commit({ phase: "generating" });
  progress.stage='generating';emit();
  async function worker() {
    while (true) {
      if (persistenceError) return;
      if (signal?.aborted) return;
      const idx = cursor++;
      if (idx >= manifest.entries.length) return;
      if (sharedImageError) {
        const entry = manifest.entries[idx];
        familiesResult[idx] = {
          family: entry.family, prompt: entry.prompt, ok: false,
          skipped: true, error: sharedImageError,
        };
        setStatus(progress, entry.family, { status: "failed", skipped: true, error: sharedImageError });
        progress.completed += 1;
        emit();
        continue;
      }
      familiesResult[idx] = await runOne(manifest.entries[idx]);
    }
  }
  const workerCount = Math.min(CONCURRENCY, manifest.entries.length);
  await Promise.all(Array.from({ length: workerCount }, () => worker()));
  if (persistenceError) throw persistenceError;

  // Abort propagation: if user cancelled, raise to caller (so we don't accidentally save).
  if (signal?.aborted) throw new DOMException("aborted", "AbortError");

  // Allow partial success: caller decides if there's at least one to save.
  progress.stage = "saving";
  progress.message = sharedImageError
    ? `${progress.referenceImageError} 已停止提交剩余 ${familiesResult.filter((family) => family.skipped).length} 个动作，成功生成 ${familiesResult.filter((family) => family.ok).length} 个动作；成功结果保留。`
    : "正在写入本地资源…";
  emit();

  return {
    persona: manifest.persona,
    manifest,
    imagePayload,
    originalImagePayload,
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
  await requireAvailableVideoModel(providerConfig, signal);
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
      cameraFixed: effectiveProviderConfig.cameraFixed,
      seed: effectiveProviderConfig.seed,
    },
    prompt,
    imageDataUrl,
    signal,
    onStage,
  });
}
