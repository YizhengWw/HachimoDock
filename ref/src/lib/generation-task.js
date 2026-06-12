/**
 * [Input] Wizard hands off `{ imageFile, providerConfig, ... }`; UI subscribes to state.
 * [Output] Module-level singleton that owns the current avatar-generation run so it
 *   survives navigation (the wizard component can unmount; the run keeps going) and reports concise provider errors.
 * [Pos] lib node in ref/src/lib
 * [Sync] If this file changes, update this header and any UI component subscribing
 *   via `subscribeGenerationTask` (currently App.jsx + AppearanceGallery.jsx).
 */

import { resolveThinkingModelName, runAvatarPipeline } from "./avatar-pipeline/run.js";
import { FAMILIES } from "./avatar-pipeline/families.js";
import { replaceFamilyVideo, saveAppearance } from "./appearance-store.js";

/**
 * @typedef {"idle" | "running" | "completed" | "failed"} TaskStatus
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
  completionEpoch: 0,
};

/** @type {AbortController | null} */
let activeController = null;

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
export function acknowledgeGenerationTask() {
  if (state.status === "completed" || state.status === "failed") {
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
  setState({
    status: "running",
    appearanceName: input.appearanceName?.trim() || "未命名形象",
    appearanceId: "",
    error: "",
    startedAt: Date.now(),
    progress: {
      stage: "thinking",
      completed: 0,
      total: FAMILIES.length,
      families: Object.fromEntries(FAMILIES.map((f) => [f.family, { status: "pending" }])),
      message: "正在分析图片并生成 prompt…",
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
  const primary = extractPrimaryError(raw);
  if (!primary) return "生成失败：未收到具体错误信息。";

  if (/火山引擎模型未开通|ModelNotOpen|has not activated the model/i.test(primary)) {
    return primary.includes("火山引擎模型未开通")
      ? primary
      : "火山引擎模型未开通：当前 Ark 账号还没有开通所选视频模型。请在 Ark 控制台开通模型服务，或先切换到已开通的 Seedance 1.5 模型。";
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
    const lines = [`失败原因：${item.message}`, `影响动作：${item.families.slice(0, 8).join("、")}`];
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
  let savedId = "";
  const onFamilyDone = async ({ persona, manifest, imagePayload, family }) => {
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
        appearanceName,
        personality,
        provider: providerConfig.provider,
        model: providerConfig.model,
        baseUrl: providerConfig.baseUrl,
        thinkingModel: resolveThinkingModelName(providerConfig),
        persona,
        imagePayload,
        families: initialFamilies,
      });
      savedId = saved.id;
      setState({ appearanceId: savedId });
    } else {
      await replaceFamilyVideo({
        appearanceId: savedId,
        family: family.family,
        videoBytes: family.videoBytes,
        taskId: family.taskId,
        videoUrl: family.videoUrl,
        prompt: family.prompt,
      });
    }
  };

  try {
    const result = await runAvatarPipeline({
      imageFile,
      appearanceName,
      personality,
      providerConfig,
      skipProcessing,
      signal: activeController.signal,
      onProgress: (p) => setState({ progress: p }),
      onFamilyDone,
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
      });
      savedId = saved.id;
    }
    setState({
      status: "completed",
      appearanceId: savedId,
      completionEpoch: state.completionEpoch + 1,
    });
  } catch (err) {
    const isAbort =
      err?.name === "AbortError" || /aborted/i.test(String(err?.message || ""));
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
      setState({ status: "idle", progress: null });
      return;
    }
    if (savedId) {
      // Pipeline failed late but earlier families landed: surface them.
      setState({
        status: "completed",
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
