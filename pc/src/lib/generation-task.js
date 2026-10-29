/**
 * [Input] Wizard hands off `{ imageFile, providerConfig, ... }`; UI subscribes to state.
 * [Output] Module-level singleton that owns the current custom avatar-generation run so it
 *   survives navigation, distinguishes reference-image moderation and skipped actions,
 *   identifies unavailable models by request stage and actual model ID,
 *   replaces conversion progress with a truthful terminal summary, and guards
 *   terminal-result acknowledgement by completion epoch.
 * [Pos] lib node in pc/src/lib
 * [Sync] If this file changes, update this header and any UI component subscribing
 *   via `subscribeGenerationTask` (currently App.jsx + AppearanceGallery.jsx).
 */

import { resolveThinkingModelName, runAvatarPipeline } from "./avatar-pipeline/run.js";
import { CUSTOM_GENERATION_FAMILIES } from "./avatar-pipeline/families.js";
import { getVolcanoInputImageRejection } from "./avatar-pipeline/providers/volcano.js";
import { getArkImageEditPolicyRejection } from "./avatar-pipeline/providers/volcano-image.js";
import { replaceFamilyVideo, saveAppearance, prepareSavedAppearance } from "./appearance-store.js";
import { recordDiagnostic } from "./avatar-pipeline/diagnostics.js";
import { loadGenerationSession, saveGenerationSession, beginGenerationSession, publicGenerationConfig, generationSessionNeedsProtection, summarizeGenerationSession, canAutoResume } from "./avatar-pipeline/session.js";
import { loadProviderConfig, loadArkImageConfig } from "./avatar-pipeline/provider-config.js";

/**
 * @typedef {"idle" | "running" | "paused" | "completed" | "failed"} TaskStatus
 *
 * @typedef {{
 *   status: TaskStatus,
 *   appearanceName: string,
 *   appearanceId: string,
 *   progress: object | null,
 *   error: string,
 *   startedAt: number,
 *   // Bumped each time we transition into a terminal state so UI can tell
 *   // a fresh completion apart from a lingering already-shown one.
 *   completionEpoch: number,
 * }} GenerationTaskState
 */

/** @type {GenerationTaskState} */
let state = {
  status: "idle",
  appearanceName: "",
  appearanceId: "",
  progress: null,
  error: "",
  startedAt: 0,
  stageStartedAt: 0,
  completionEpoch: 0,
};

/** @type {AbortController | null} */
let activeController = null;
let gateResolve = null;
let initialization = null;
let resumeTimer = null;
function scheduleResume(session) {
  clearTimeout(resumeTimer);
  if(!canAutoResume(session))return;
  const id=session.id;
  resumeTimer=setTimeout(async()=>{
    try {
      const current=await loadGenerationSession();
      if(current?.id!==id||isGenerationRunning()||!canAutoResume(current))return;
      current.autoResumes=(current.autoResumes||0)+1;await saveGenerationSession(current);
      await resumeGenerationTask('current', { automaticResume: true });
    }catch(error){console.warn('generation automatic recovery stopped',error.message);}
  },30000*Math.pow(2,session.autoResumes||0));
}

export function confirmGenerationStage() { gateResolve?.(); }

async function awaitPreview(kind, bytes, mime, message, signal) {
  if (signal.aborted) throw new DOMException("aborted", "AbortError");
  const url = URL.createObjectURL(new Blob([bytes], { type: mime }));
  setState({ awaiting: { kind, url, message } });
  try {
    await new Promise((resolve, reject) => {
      const aborted = () => { gateResolve = null; reject(new DOMException("aborted", "AbortError")); };
      gateResolve = () => { signal.removeEventListener("abort", aborted); gateResolve = null; resolve(); };
      signal.addEventListener("abort", aborted, { once: true });
    });
  } finally { URL.revokeObjectURL(url); setState({ awaiting: null }); }
}

export function initializeGenerationRecovery() {
  if (!initialization) initialization = loadGenerationSession().then(session => {
    if (session && !session.finished && !session.dismissed && state.status === "idle") {
      const summary = summarizeGenerationSession(session);
      setState({ status: summary.status, recoveryLabel: summary.label, resumable: true,
        automatic: session.automatic === true,
        appearanceName: session.appearanceName, startedAt: session.createdAt || 0,
        appearanceId: session.appearanceId || "", error: "",
        progress: { ...summary, stage: session.phase || "paused", message: `${summary.label}。已保存 ${summary.completed}/${summary.total} 段。恢复复用原参考图及已有结果，不会自动重发结果未知的请求。${session.skipProcessing ? "此旧任务使用原图，恢复不会重新去背景；需要清理背景请另建任务，已有视频保留。" : ""}` } });
      scheduleResume(session);
    }
  }).catch(error => { console.warn("generation recovery unavailable", error.message); });
  return initialization;
}

export async function resumeGenerationTask(key = "current", { automaticResume = false } = {}) {
  if (isGenerationRunning()) return;
  const session = await loadGenerationSession(key);
  if (!session || session.finished || session.dismissed) return;
  if (!["volcengine", "kling"].includes(session.config.provider)) throw new Error("自定义接口任务暂不支持跨重启恢复，避免将旧任务发送到不同服务；请在原服务核对结果。");
  const provider = session.config.provider;
  const saved = loadProviderConfig(provider);
  const imageFile=session.original?.bytes?.byteLength
    ? new File([session.original.bytes],session.original.filename||'reference.png',{type:session.original.mime||'image/png'})
    : session.imageFile;
  startGenerationTask({ imageFile, appearanceName: session.appearanceName,
    personality: session.personality, styleDescription: session.styleDescription || '', skipProcessing: session.skipProcessing, session, restoreArchived: key !== "current",
    manualReview: session.automatic !== true,
    retryReview: !automaticResume,
    providerConfig: { ...saved, ...session.config, imageEdit: loadArkImageConfig() },
  });
}

export async function dismissGenerationSession() {
  if (isGenerationRunning()) return;
  const session = await loadGenerationSession();
  if (session) { session.dismissed = true; await saveGenerationSession(session); }
  setState({ status: "idle", resumable: false, progress: null, error: "" });
}

/** @type {Set<(s: GenerationTaskState) => void>} */
const listeners = new Set();

function notify() {
  for (const fn of listeners) {
    try {
      fn(state);
    } catch (err) {
      // A subscriber blowing up should not stop the others from updating.
      console.error("generation-task subscriber error:", err);
    }
  }
}

function setState(patch) {
  const now = Date.now();
  const oldStage = state.progress?.stage;
  const nextStage = patch.progress?.stage;
  if (state.status === "running" && oldStage && ((nextStage && nextStage !== oldStage) || (patch.status && patch.status !== "running"))) {
    recordDiagnostic({ timestampMs: now, stage: oldStage, phase: "stage", elapsedMs: now - state.stageStartedAt, outcome: patch.status === "failed" ? "failed" : "stage" });
  }
  if (nextStage && (nextStage !== oldStage || patch.status === "running")) patch = { ...patch, stageStartedAt: now };
  state = { ...state, ...patch };
  notify();
}

export function getGenerationTask() {
  return state;
}

export function subscribeGenerationTask(fn) {
  listeners.add(fn);
  fn(state);
  return () => listeners.delete(fn);
}

export function isGenerationRunning() {
  return state.status === "running";
}

export function abortGenerationTask() {
  if (activeController) activeController.abort();
}

/**
 * Clear a terminal (completed / failed) state back to idle. Safe to call from
 * a toast dismiss handler. No-op while a run is still in progress.
 */
export function acknowledgeGenerationTask(expectedCompletionEpoch = null) {
  const matchesExpectedRun =
    expectedCompletionEpoch === null || state.completionEpoch === expectedCompletionEpoch;
  if (matchesExpectedRun && ["completed", "failed", "paused"].includes(state.status)) {
    setState({
      status: "idle",
      progress: null,
      error: "",
      // Keep appearanceId for the toast's "查看" handler that fires right after
      // ack; that handler reads it before the next state mutation clobbers it.
    });
  }
}

/**
 * Kick off a generation run. Returns immediately; observe progress / completion
 * through `subscribeGenerationTask`.
 *
 * Throws synchronously if a run is already in progress — single-task model.
 */
export function startGenerationTask(input) {
  if (state.status === "running") {
    throw new Error("已有生成任务在进行中，请等待完成后再开始新的生成。");
  }

  activeController = new AbortController();
  clearTimeout(resumeTimer);
  setState({
    status: "running",
    appearanceName: input.appearanceName?.trim() || "未命名形象",
    appearanceId: "",
    error: "",
    resumable: false,
    recoveryLabel: "",
    automatic: input.manualReview !== true,
    startedAt: Date.now(),
    progress: {
      stage: "validating",
      completed: 0,
      total: CUSTOM_GENERATION_FAMILIES.length,
      families: Object.fromEntries(CUSTOM_GENERATION_FAMILIES.map((f) => [f.family, { status: "pending" }])),
      message: "正在检查模型配置…",
    },
  });

  // Don't `await` — fire-and-forget. Errors are funneled into state.
  void runTask(input).catch((err) => {
    console.error("generation-task unexpected error:", err);
    setState({
      status: "failed",
      error: normalizeGenerationErrorMessage(err?.message || String(err)),
      completionEpoch: state.completionEpoch + 1,
    });
  });
}

function compactText(value) {
  return String(value || "").replace(/[ \t]+/g, " ").replace(/\n{3,}/g, "\n\n").trim();
}

function truncateMessage(message, limit = 700) {
  const text = compactText(message);
  return text.length > limit ? `${text.slice(0, limit)}...` : text;
}

function stripDiagnosticTail(message) {
  return String(message || "")
    .split(/\nVolcano payload summary:/)[0]
    .split(/\n诊断摘要：/)[0]
    .trim();
}

function extractPrimaryError(message) {
  const text = stripDiagnosticTail(message);
  const originalIndex = text.indexOf("\n原始错误");
  return originalIndex >= 0 ? text.slice(0, originalIndex).trim() : text.trim();
}

export function normalizeGenerationErrorMessage(error) {
  const raw = compactText(error);
  if (raw.startsWith("生成失败：所有动作都没有生成成功。")) {
    return truncateMessage(raw, 1200);
  }
  const imageEditRejection = getArkImageEditPolicyRejection(raw);
  if (imageEditRejection) return imageEditRejection.message;
  const imageRejection = getVolcanoInputImageRejection(raw);
  if (imageRejection) return imageRejection.message;
  const primary = extractPrimaryError(raw);
  if (!primary) return "生成失败：未收到具体错误信息。";

  if (/模型未开通|模型不可用|ModelNotOpen|InvalidEndpointOrModel\.NotFound|has not activated the model/i.test(primary)) {
    const requestModel = /(?:Ark image edit|thinking model|参考图检查|试片检查)\s*\(([^)\n]+)\)/i.exec(primary)?.[1];
    const namedModel = /\b(?:doubao-[a-z0-9][a-z0-9._-]*|ep-[a-z0-9][a-z0-9_-]*)\b/i.exec(primary)?.[0];
    const diagnosticModel = /"model"\s*:\s*"([^"\n]+)"/.exec(raw.split("\nVolcano payload summary:")[1] || "")?.[1];
    const model = requestModel || namedModel || diagnosticModel || "模型 ID 未获取";
    const stage = /Ark image edit|图片处理|seedream/i.test(primary) ? "图片处理"
      : /参考图检查|试片检查|自动检查/.test(primary) ? "自动检查"
      : /thinking model|图像理解/i.test(primary) ? "图像理解 / 提示词"
      : /volcano (?:submit|task)|视频生成|视频模型|seedance/i.test(primary) ? "视频生成" : "形象生成";
    const reason = /InvalidEndpointOrModel\.NotFound|模型不可用/i.test(primary) ? "模型不可用" : "模型未开通或无权限";
    return `${stage}${reason}：${model}。请在 Ark 控制台确认该模型已开通、未下线，且当前 Key 有权调用。`;
  }

  if (/expected the height to be at least 300px/i.test(primary)) {
    return "参考图尺寸不满足火山引擎要求：图片高度至少需要 300px。请重新上传更高分辨率的参考图，或开启当前的 400x300 快速参考帧处理。";
  }

  if (/Model do not support image input|do not support image input/i.test(primary)) {
    return "文字分析模型不支持图片输入：请使用 Doubao 2.0 多模态 Responses 模型生成 prompt，不要把 Seedance 或 DeepSeek 文本模型作为图片分析模型。";
  }

  if (/HTTP 401|Unauthorized|InvalidAuthentication|invalid api key|api key/i.test(primary)) {
    return "API Key 无效或已过期：请检查火山引擎 Ark API Key 是否复制完整，并确认当前账号有对应服务权限。";
  }

  if (/HTTP 403|Forbidden|AccessDenied|PermissionDenied/i.test(primary)) {
    return "账号没有访问权限：请检查 Ark 控制台中该模型服务、地域和账号权限是否已开通。";
  }

  if (/HTTP 429|RateLimit|TooManyRequests|quota|insufficient|余额|额度/i.test(primary)) {
    return "请求被限流或额度不足：请稍后重试，或检查 Ark 控制台的并发限制、用量额度和账户余额。";
  }

  if (/InvalidParameter|BadRequest|HTTP 400/i.test(primary)) {
    return `请求参数不被当前模型接受：请检查所选模型是否支持当前图生视频输入、首尾帧角色、比例和时长。${truncateMessage(primary, 360)}`;
  }

  if (/Failed to fetch|NetworkError|无法连接|timeout|timed out/i.test(primary)) {
    return `网络或服务连接失败：请检查网络、代理和 Ark 服务地址后重试。${truncateMessage(primary, 360)}`;
  }

  return truncateMessage(primary);
}

function diagnosticLine(message) {
  const text = String(message || "");
  const summary = text.split("\nVolcano payload summary:")[1];
  if (summary) return `诊断摘要：${truncateMessage(summary, 500)}`;
  return "";
}

export function summarizeFamilyFailures(families) {
  const failedFamilies = families.filter((family) => !family.ok && family.error);
  if (!failedFamilies.length) return "";

  const groups = new Map();
  for (const family of failedFamilies) {
    const message = normalizeGenerationErrorMessage(family.error);
    const key = message.replace(/Request id:\s*\S+/gi, "Request id:<redacted>");
    const current = groups.get(key) || { message, families: [], diagnostic: "" };
    current.families.push(family.family);
    if (!current.diagnostic) current.diagnostic = diagnosticLine(family.error);
    groups.set(key, current);
  }

  const unique = Array.from(groups.values());
  if (unique.length === 1) {
    const [item] = unique;
    const lines = [`失败原因：${item.message}`, `影响动作：${item.families.join("、")}`];
    const skipped = failedFamilies.filter((family) => family.skipped).length;
    if (skipped) lines.push(`已停止提交剩余 ${skipped} 个动作（共用参考图未通过审核）；已完成的结果保留。`);
    if (item.diagnostic) lines.push(item.diagnostic);
    return `\n${lines.join("\n")}`;
  }

  const failures = unique.slice(0, 3).map((item) => {
    const familyLabel = item.families.slice(0, 3).join("、");
    return `${familyLabel}: ${truncateMessage(item.message, 420)}`;
  });
  return failures.length ? `\n失败详情：\n${failures.join("\n")}` : "";
}

export function buildAllFamiliesFailedMessage(families) {
  return `生成失败：所有动作都没有生成成功。${summarizeFamilyFailures(families)}`;
}

async function runTask(input) {
  const {
    imageFile,
    appearanceName,
    personality,
    providerConfig,
    // Forwarded to runAvatarPipeline so the wizard's "remove background"
    // toggle still controls image preprocessing now that the runner lives
    // outside the component.
    skipProcessing,
  } = input;

  // Incremental persistence mirrors the previous in-wizard behaviour: create
  // the appearance record on the first successful family, then append each
  // subsequent one. Keeps earlier successes safe even if the run is aborted
  // or the final family stalls in the provider's queue.
  const session = input.session || {
    id: crypto.randomUUID(), config: publicGenerationConfig(providerConfig), imageFile,
    appearanceName, personality, styleDescription: input.styleDescription || '', skipProcessing, actions: {}, createdAt: Date.now(),
    distinctStartFrames: !skipProcessing && providerConfig.provider==='volcengine',
    ...(input.referencePreview?.bytes?.byteLength && !skipProcessing ? {reference:input.referencePreview} : {}),
    automatic: input.manualReview !== true,
  };
  session.dismissed = false;
  session.pausedByUser = false;
  if (input.session && !input.restoreArchived) await saveGenerationSession(session);
  else await beginGenerationSession(session);
  let savedId = session.appearanceId || "";
  const onFamilyDone = async ({ persona, manifest, imagePayload, originalImagePayload, family }) => {
    if (!savedId) {
      const initialFamilies = manifest.entries.map((entry) =>
        entry.family === family.family
          ? {
              family: family.family,
              ok: true,
              prompt: entry.prompt,
              videoBytes: family.videoBytes,
              taskId: family.taskId,
              videoUrl: family.videoUrl,
            }
          : { family: entry.family, ok: false, prompt: entry.prompt, error: "pending" },
      );
      const saved = await saveAppearance({
        appearanceId: session.id,
        appearanceName,
        personality,
        provider: providerConfig.provider,
        model: providerConfig.model,
        baseUrl: providerConfig.baseUrl,
        thinkingModel: resolveThinkingModelName(providerConfig),
        persona,
        imagePayload,
        originalImagePayload,
        families: initialFamilies,
        deferPreparation: true,
      });
      savedId = saved.id;
      session.appearanceId = savedId;
      await saveGenerationSession(session);
      setState({ appearanceId: savedId });
    } else {
      await replaceFamilyVideo({
        appearanceId: savedId,
        family: family.family,
        videoBytes: family.videoBytes,
        taskId: family.taskId,
        videoUrl: family.videoUrl,
        prompt: family.prompt,
        deferPreparation: true,
      });
    }
  };

  try {
    if(['converting','conversion_failed'].includes(session.phase)&&savedId){
      setState({progress:{...summarizeGenerationSession(session),stage:'saving',message:'复用已保存动画，正在准备设备格式…'}});
      await prepareSavedAppearance(savedId);
      session.phase=session.allVideosReady?'completed':'partial';session.finished=!!session.allVideosReady;
      await saveGenerationSession(session);
      setState({status:'completed',resumable:!session.finished,appearanceId:savedId,
        progress:{...summarizeGenerationSession(session),stage:'completed',message:session.finished?'整套动画和设备格式已准备完成。':'已生成部分和设备格式已保存，可继续恢复剩余动作。'},
        completionEpoch:state.completionEpoch+1});return;
    }
    const result = await runAvatarPipeline({
      styleDescription:session.styleDescription || '',
      distinctStartFrames:session.distinctStartFrames === true,
      imageFile,
      appearanceName,
      personality,
      providerConfig,
      skipProcessing,
      signal: activeController.signal,
      onProgress: (p) => setState({ progress: p }),
      onFamilyDone,
      session,
      checkpoint: saveGenerationSession,
      automatic: session.automatic,
      retryReview: input.retryReview === true,
      confirmReference: session.automatic ? undefined : (reference) => awaitPreview("reference", reference.bytes, reference.mime,
        skipProcessing ? "当前直接使用原图，请确认主体、背景和杂物均符合预期。确认后生成提示词和一段试片。"
          : "请确认原人物或角色全身和随身物件完整，主体自身的手脚保留，无关背景、旁人和杂物已移除，背景为纯黑色。不符合时请取消，不继续生成视频。", activeController.signal),
      confirmTrial: session.automatic ? undefined : (family, remaining) => awaitPreview("trial", family.videoBytes, "video/mp4",
        `请检查角色、动作和首尾衔接。确认后才继续生成剩余 ${remaining} 段（会产生费用）。`, activeController.signal),
    });
    const successes = result.families.filter((f) => f.ok).length;
    if (successes === 0) {
      throw new Error(buildAllFamiliesFailedMessage(result.families));
    }
    if (!savedId) {
      // Fallback path — `onFamilyDone` should already have saved, but keep
      // this so we never silently lose a successful run.
      const saved = await saveAppearance({
        appearanceName,
        personality,
        provider: providerConfig.provider,
        model: providerConfig.model,
        baseUrl: providerConfig.baseUrl,
        thinkingModel: resolveThinkingModelName(providerConfig),
        persona: result.persona,
        imagePayload: result.imagePayload,
        families: result.families,
        deferPreparation: true,
      });
      savedId = saved.id;
    }
    session.allVideosReady = result.families.every(f => f.ok);
    session.phase='converting';await saveGenerationSession(session);
    setState({progress:{...state.progress,stage:'saving',message:'动画已保存，正在准备设备格式…'}});
    await prepareSavedAppearance(savedId);
    session.finished = session.allVideosReady;
    session.phase = session.finished ? "completed" : "partial";
    if(Object.values(session.actions).some(a=>a.deferred))session.phase='waiting_network';
    await saveGenerationSession(session);
    setState({
      status: session.phase==='waiting_network' ? "paused" : "completed",
      recoveryLabel: session.phase==='waiting_network' ? '等待网络恢复' : '',
      progress: {...summarizeGenerationSession(session),stage:session.phase==='waiting_network'?'waiting_network':'completed',
        message:session.finished?'整套动画和设备格式已准备完成。':session.phase==='waiting_network'?'已有动画和设备格式已保存，等待网络恢复后继续。':'已生成部分和设备格式已保存，可继续恢复剩余动作。'},
      resumable: !session.finished,
      appearanceId: savedId,
      completionEpoch: state.completionEpoch + 1,
    });
    scheduleResume(session);
  } catch (err) {
    const isAbort = err?.name === "AbortError" || /aborted/i.test(String(err?.message || ""));
    session.pausedByUser=isAbort;
    const deferred=Object.values(session.actions||{}).some(a=>a.deferred);
    if (!isAbort) session.phase = session.phase==='converting' ? 'conversion_failed' : deferred ? 'waiting_network' : "failed";
    const protectedWork = generationSessionNeedsProtection(session);
    if (err?.code === 'PROMPT_OUTPUT_INVALID') err.message += ' 点击恢复可单独重新生成提示词（会产生费用），不重复处理参考图。';
    if (!protectedWork) {
      session.dismissed = true;
      session.terminalFailure = true;
    }
    await saveGenerationSession(session);
    scheduleResume(session);
    setState({ resumable: protectedWork, progress: { ...state.progress,
      ...summarizeGenerationSession(session), message: err?.message || '生成暂停，已有成果保留' } });
    if (isAbort && savedId) {
      // Partial run that the user cancelled — surface what was persisted.
      setState({
        status: "completed",
        appearanceId: savedId,
        completionEpoch: state.completionEpoch + 1,
      });
      return;
    }
    if (isAbort) {
      // Cancelled before anything saved — just go back to idle.
      setState({ status: "failed", progress: null, error: protectedWork
        ? "生成已停止，已提交任务进度已保留，可恢复。"
        : "生成已停止，尚未提交视频任务，可以直接新建。" });
      return;
    }
    if(deferred){setState({status:'paused',recoveryLabel:'等待网络恢复',error:'',appearanceId:savedId,progress:{...state.progress,message:'暂时无法取得云端结果，稍后自动继续查询；不会重新提交已知任务。'}});return;}
    if (savedId) {
      // Pipeline failed late but earlier families landed: surface them.
      setState({
        status: "failed",
        error: normalizeGenerationErrorMessage(err?.message || String(err)),
        appearanceId: savedId,
        completionEpoch: state.completionEpoch + 1,
      });
      return;
    }
    setState({
      status: "failed",
      error: normalizeGenerationErrorMessage(err?.message || String(err)),
      completionEpoch: state.completionEpoch + 1,
    });
  } finally {
    activeController = null;
  }
}
