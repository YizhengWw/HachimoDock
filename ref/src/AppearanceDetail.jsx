/**
 * [Input] An appearanceId persisted by `lib/appearance-store.js`.
 * [Output] Preview-first appearance workspace, sticky current-state controls, contextual details drawer,
 *          configurable per-family WAV cue overrides, direct per-state MP4 replacement,
 *          background single-state image+prompt regeneration that only replaces the client video
 *          before manual board downlink, inline progress, and full known-state rail.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  ArrowLeft,
  Trash2,
  AlertCircle,
  Loader,
  Upload,
  UploadCloud,
  Volume2,
  ImageUp,
} from "lucide-react";
import {
  deleteAppearance,
  getAppearance,
  readAudioAsBlobUrl,
  removeFamilyAudioCue,
  replaceFamilyAudioCue,
  replaceFamilyVideo,
} from "./lib/appearance-store.js";
import AppearancePreview from "./AppearancePreview.jsx";
import { compressWavFileForBoard } from "./lib/audio-cue.js";
import { mediaFromFamily, mediaFromSourceImage, mediaFromSourcePreview } from "./lib/appearance-preview.js";
import { FAMILIES } from "./lib/avatar-pipeline/families.js";
import { runSingleFamilyVideo } from "./lib/avatar-pipeline/run.js";
import {
  DEFAULT_ADVANCED,
  DEFAULT_PROVIDER_ID,
  VIDEO_PROVIDERS,
  VOLCENGINE_BASE_URL,
  VOLCENGINE_THINKING_MODEL,
  loadProviderConfig,
  saveProviderConfig,
} from "./lib/avatar-pipeline/provider-config.js";
import { AvatarWizardStep1, AvatarWizardStep2 } from "./CustomAvatarWizard.jsx";
import { hasTauriRuntime } from "./lib/tauri-env.js";

const AUDIO_SYNC_DIRTY_PREFIX = "pet-manager.appearance-audio-dirty.";

function appearanceSourceLabel(record) {
  if (record.type === "builtin") return "内置形象";
  if (record.type === "codex-import") return "codex pet";
  return "自定义形象";
}

function dirtyStorageKey(appearanceId) {
  return `${AUDIO_SYNC_DIRTY_PREFIX}${appearanceId}`;
}

function readAudioSyncDirty(appearanceId) {
  if (!appearanceId || typeof window === "undefined" || !window.localStorage) return false;
  return window.localStorage.getItem(dirtyStorageKey(appearanceId)) === "1";
}

function writeAudioSyncDirty(appearanceId, dirty) {
  if (!appearanceId || typeof window === "undefined" || !window.localStorage) return;
  const key = dirtyStorageKey(appearanceId);
  if (dirty) window.localStorage.setItem(key, "1");
  else window.localStorage.removeItem(key);
}

function isImageFile(file) {
  if (!file) return false;
  if (/^image\//i.test(file.type || "")) return true;
  return /\.(png|jpe?g|webp|gif)$/i.test(file.name || "");
}

function isMp4VideoFile(file) {
  if (!file) return false;
  if ((file.type || "").toLowerCase() === "video/mp4") return true;
  return /\.mp4$/i.test(file.name || "");
}

async function readFileAsBytes(file) {
  return new Uint8Array(await file.arrayBuffer());
}

function clampPercent(value) {
  if (!Number.isFinite(value)) return 0;
  return Math.max(0, Math.min(100, value));
}

function percentFromUnitProgress(value, min, max) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) return min;
  const normalized = numeric > 1 ? numeric / 100 : numeric;
  return Math.round(min + Math.max(0, Math.min(1, normalized)) * (max - min));
}

function singleStateProgressFromPipeline(progress, fallbackFamily) {
  const stage = progress?.stage || "processing";
  const status = progress?.status || "";
  const family = progress?.family || fallbackFamily || "当前状态";
  const message = progress?.message || `正在生成 ${family} 状态素材…`;

  if (stage === "processing") {
    const percent = typeof progress?.progress === "number"
      ? percentFromUnitProgress(progress.progress, 12, 34)
      : message.includes("完成")
        ? 36
        : message.includes("合成")
          ? 30
          : 12;
    return { label: "处理参考图", percent: clampPercent(percent), message, tone: "running" };
  }
  if (stage === "generating") {
    const statusPercent = {
      submitting: 44,
      polling: 64,
      downloading: 84,
      done: 94,
    };
    return {
      label: status === "downloading" ? "下载生成结果" : "生成状态视频",
      percent: clampPercent(statusPercent[status] || 50),
      message,
      tone: "running",
    };
  }
  if (stage === "done") {
    return { label: "写入客户端素材", percent: 94, message, tone: "running" };
  }
  return { label: "生成中", percent: 24, message, tone: "running" };
}

export default function AppearanceDetail({ appearanceId, onBack }) {
  const [record, setRecord] = useState(null);
  const [error, setError] = useState("");
  const [activeFamily, setActiveFamily] = useState("");
  const [audioUrl, setAudioUrl] = useState("");
  const [audioErr, setAudioErr] = useState("");
  const [audioState, setAudioState] = useState("idle");
  const [stateVideoState, setStateVideoState] = useState("idle");
  const [stateVideoMessage, setStateVideoMessage] = useState("");
  const [audioSyncDirty, setAudioSyncDirty] = useState(() => readAudioSyncDirty(appearanceId));
  const [audioSyncState, setAudioSyncState] = useState("idle"); // idle | syncing | success | error
  const [audioSyncMessage, setAudioSyncMessage] = useState("");
  const [deleteState, setDeleteState] = useState("idle"); // idle | confirm | deleting
  const [deleteError, setDeleteError] = useState("");
  const [singleStateImageFile, setSingleStateImageFile] = useState(null);
  const [singleStateImagePreview, setSingleStateImagePreview] = useState("");
  const [singleStatePrompt, setSingleStatePrompt] = useState("");
  const [singleStateStatus, setSingleStateStatus] = useState("idle"); // idle | generating | success | error | syncing | synced
  const [singleStateMessage, setSingleStateMessage] = useState("");
  const [singleStateProgress, setSingleStateProgress] = useState(null);
  const [singleStateDialogOpen, setSingleStateDialogOpen] = useState(false);
  const [singleStateStep, setSingleStateStep] = useState(0);
  const singleStateInputRef = useRef(null);
  const [providerId, setProviderId] = useState(DEFAULT_PROVIDER_ID);
  const provider = useMemo(
    () => VIDEO_PROVIDERS.find((item) => item.id === providerId) || VIDEO_PROVIDERS[0],
    [providerId],
  );
  const isVolcengine = providerId === "volcengine";
  const [apiKey, setApiKey] = useState("");
  const [accessKey, setAccessKey] = useState("");
  const [secretKey, setSecretKey] = useState("");
  const [baseUrl, setBaseUrl] = useState(provider.baseUrl);
  const [model, setModel] = useState(provider.models[0] || "");
  const [thinkingModel, setThinkingModel] = useState("");
  const [advancedOpen, setAdvancedOpen] = useState(false);
  const [openaiCompat, setOpenaiCompat] = useState(true);
  const [advanced, setAdvanced] = useState({ ...DEFAULT_ADVANCED });
  const [testFeedback, setTestFeedback] = useState(null);
  const [removeBg, setRemoveBg] = useState(true);
  const [fastGeneration, setFastGeneration] = useState(true);

  const stateFamilyRecords = useMemo(() => {
    if (!record) return [];
    const stored = new Map((record.families || []).map((family) => [family.family, family]));
    const known = new Set(FAMILIES.map((definition) => definition.family));
    const merged = FAMILIES.map((definition) => (
      stored.get(definition.family) || {
        family: definition.family,
        ok: false,
        prompt: "",
        error: "尚未上传状态视频",
      }
    ));
    for (const family of record.families || []) {
      if (!known.has(family.family)) merged.push(family);
    }
    return merged;
  }, [record]);

  const familyByName = useMemo(() => {
    const map = new Map();
    for (const family of stateFamilyRecords) map.set(family.family, family);
    return map;
  }, [stateFamilyRecords]);

  const activeRecord = familyByName.get(activeFamily);

  const generatedFamilies = useMemo(
    () => stateFamilyRecords.filter((family) => family.ok),
    [stateFamilyRecords],
  );

  const audioFamilyNames = useMemo(
    () => generatedFamilies
      .filter((family) => family.audioPath || family.audioSrc)
      .map((family) => family.family),
    [generatedFamilies],
  );

  useEffect(() => {
    setSingleStatePrompt(activeRecord?.prompt || "");
    setSingleStateImageFile(null);
    setSingleStateMessage("");
    setSingleStateProgress(null);
    setSingleStateStatus("idle");
    setStateVideoState("idle");
    setStateVideoMessage("");
    setSingleStateDialogOpen(false);
    setSingleStateStep(0);
  }, [activeRecord?.family, record?.id]);

  useEffect(() => {
    if (record?.provider && VIDEO_PROVIDERS.some((item) => item.id === record.provider)) {
      setProviderId(record.provider);
    }
  }, [record?.provider]);

  useEffect(() => {
    const saved = loadProviderConfig(providerId);
    setApiKey(saved.apiKey);
    setAccessKey(saved.accessKey);
    setSecretKey(saved.secretKey);
    setBaseUrl(saved.baseUrl);
    setModel(saved.model);
    setThinkingModel(saved.thinkingModel);
    setFastGeneration(saved.fastGeneration);
    setAdvanced(saved.advanced);
    setTestFeedback(null);
  }, [providerId]);

  useEffect(() => {
    if (!singleStateImageFile) {
      setSingleStateImagePreview("");
      return undefined;
    }
    const reader = new FileReader();
    let cancelled = false;
    reader.onload = () => {
      if (!cancelled) setSingleStateImagePreview(typeof reader.result === "string" ? reader.result : "");
    };
    reader.onerror = () => {
      if (!cancelled) setSingleStateImagePreview("");
    };
    reader.readAsDataURL(singleStateImageFile);
    return () => {
      cancelled = true;
      if (reader.readyState === FileReader.LOADING) {
        try {
          reader.abort();
        } catch {
          /* noop */
        }
      }
    };
  }, [singleStateImageFile]);

  const markAudioSyncDirty = useCallback((message) => {
    writeAudioSyncDirty(appearanceId, true);
    setAudioSyncDirty(true);
    setAudioSyncState("idle");
    setAudioSyncMessage(message || "音效已保存到客户端，需要通过板端音效 OTA 通道下发到设备后才会生效。");
  }, [appearanceId]);

  const reload = useCallback(async () => {
    setError("");
    try {
      const nextRecord = await getAppearance(appearanceId);
      setRecord(nextRecord);
      const firstOk = nextRecord.families.find((family) => family.ok);
      setActiveFamily(firstOk?.family || nextRecord.families[0]?.family || FAMILIES[0]?.family || "");
    } catch (loadError) {
      setError(loadError?.message || String(loadError));
    }
  }, [appearanceId]);

  useEffect(() => {
    reload();
  }, [reload]);

  useEffect(() => {
    setDeleteState("idle");
    setDeleteError("");
    setAudioSyncDirty(readAudioSyncDirty(appearanceId));
    setAudioSyncState("idle");
    setAudioSyncMessage("");
  }, [appearanceId]);

  useEffect(() => {
    if (!record || !activeFamily) {
      setAudioUrl("");
      return undefined;
    }

    let cancelled = false;
    let createdObjectUrl = "";

    (async () => {
      setAudioErr("");
      setAudioUrl("");
      const family = record.families.find((item) => item.family === activeFamily);
      if (!family || !family.ok) return;

      if (family.audioSrc) {
        setAudioUrl(family.audioSrc);
        return;
      }

      if (!family.audioPath) return;

      try {
        const url = await readAudioAsBlobUrl(family.audioPath);
        if (cancelled) {
          URL.revokeObjectURL(url);
        } else {
          createdObjectUrl = url;
          setAudioUrl(url);
        }
      } catch (loadError) {
        if (!cancelled) setAudioErr(loadError?.message || String(loadError));
      }
    })();

    return () => {
      cancelled = true;
      if (createdObjectUrl) URL.revokeObjectURL(createdObjectUrl);
    };
  }, [record, activeFamily]);

  const handleDelete = useCallback(async () => {
    if (!record || deleteState === "deleting") return;
    if (deleteState !== "confirm") {
      setDeleteState("confirm");
      return;
    }
    setDeleteState("deleting");
    setDeleteError("");
    try {
      await deleteAppearance(record.id);
      onBack?.();
    } catch (deleteFailure) {
      console.error(deleteFailure);
      setDeleteError(deleteFailure?.message || String(deleteFailure));
      setDeleteState("idle");
    }
  }, [deleteState, record, onBack]);

  const handleAudioFileChange = useCallback(async (event) => {
    const file = event.target.files?.[0];
    event.target.value = "";
    if (!file || !record || !activeFamily || audioState === "saving") return;
    if (!file.name.toLowerCase().endsWith(".wav") && !/audio\/(x-)?wav/i.test(file.type || "")) {
      setAudioErr("请上传 WAV 格式文件");
      return;
    }
    setAudioState("saving");
    setAudioErr("");
    try {
      const audioBytes = await compressWavFileForBoard(file);
      const nextRecord = await replaceFamilyAudioCue({
        appearanceId: record.id,
        family: activeFamily,
        audioBytes,
      });
      setRecord(nextRecord);
      markAudioSyncDirty(`已更新 ${activeFamily} 的提示音，请通过音效通道下发到设备。`);
    } catch (saveFailure) {
      console.error(saveFailure);
      setAudioErr(saveFailure?.message || String(saveFailure));
    } finally {
      setAudioState("idle");
    }
  }, [activeFamily, audioState, markAudioSyncDirty, record]);

  const handleRemoveAudioCue = useCallback(async () => {
    if (!record || !activeFamily || audioState === "saving") return;
    setAudioState("saving");
    setAudioErr("");
    try {
      const nextRecord = await removeFamilyAudioCue({ appearanceId: record.id, family: activeFamily });
      setRecord(nextRecord);
      markAudioSyncDirty(`已移除 ${activeFamily} 的提示音，请通过音效通道下发到设备。`);
    } catch (removeFailure) {
      console.error(removeFailure);
      setAudioErr(removeFailure?.message || String(removeFailure));
    } finally {
      setAudioState("idle");
    }
  }, [activeFamily, audioState, markAudioSyncDirty, record]);

  const handleStateVideoFileChange = useCallback(async (event) => {
    const file = event.target.files?.[0];
    event.target.value = "";
    if (!file || !record || !activeFamily || stateVideoState === "saving") return;
    if (record.type === "builtin") {
      setStateVideoState("error");
      setStateVideoMessage("内置形象不能直接替换状态视频，请先创建自定义形象后再编辑。");
      return;
    }
    if (!isMp4VideoFile(file)) {
      setStateVideoState("error");
      setStateVideoMessage("请上传 MP4 格式的状态视频。");
      return;
    }
    setStateVideoState("saving");
    setStateVideoMessage("正在写入客户端状态视频...");
    try {
      const videoBytes = await readFileAsBytes(file);
      const nextRecord = await replaceFamilyVideo({
        appearanceId: record.id,
        family: activeFamily,
        videoBytes,
        taskId: `local-upload-${Date.now().toString(36)}`,
        videoUrl: file.name,
        prompt: activeRecord?.prompt || `用户上传的 ${activeFamily} 状态视频`,
      });
      setRecord(nextRecord);
      const message = `已上传并替换 ${activeFamily} 状态视频。需要在设备生效时，点击“替换到板端”。`;
      setStateVideoState("success");
      setStateVideoMessage(message);
      setSingleStateStatus("success");
      setSingleStateMessage(message);
      setSingleStateProgress(null);
    } catch (replaceFailure) {
      console.error(replaceFailure);
      setStateVideoState("error");
      setStateVideoMessage(replaceFailure?.message || String(replaceFailure));
    }
  }, [activeFamily, activeRecord?.prompt, record, stateVideoState]);

  const handleSyncAudioCues = useCallback(async () => {
    if (!record || audioSyncState === "syncing") return;
    if (!hasTauriRuntime()) {
      setAudioSyncState("error");
      setAudioSyncMessage("当前不是桌面客户端运行环境，无法通过板端音效 OTA 通道下发。");
      return;
    }

    setAudioSyncState("syncing");
    setAudioSyncMessage("正在通过板端音效 OTA 通道下发 WAV...");
    try {
      const result = await invoke("usb_sync_appearance", { appearanceId: record.id });
      if (!result?.ok) {
        throw new Error(result?.error || "设备未确认提示音下发");
      }
      writeAudioSyncDirty(record.id, false);
      setAudioSyncDirty(false);
      setAudioSyncState("success");
      setAudioSyncMessage(
        `音效已下发到设备（${result.fileCount || 0} 个素材，${result.byteCount || 0} bytes）。`,
      );
    } catch (syncFailure) {
      console.error(syncFailure);
      setAudioSyncState("error");
      setAudioSyncMessage(syncFailure?.message || String(syncFailure));
    }
  }, [audioSyncState, record]);

  const singleStateGenerationReadyIssue = useMemo(() => {
    if (providerId === "kling") {
      if (!accessKey.trim() || !secretKey.trim()) {
        return "请先填写 Kling Access Key 和 Secret Key。";
      }
      if (!baseUrl.trim() || !model.trim()) {
        return "请先填写接口地址和视频生成模型。";
      }
      return "";
    }

    if (isVolcengine && (!apiKey.trim() || !model.trim())) {
      return "请先填写 API Key 和视频生成模型。";
    }
    if (!apiKey.trim()) return "请先填写 API Key。";
    if (!baseUrl.trim() || !model.trim()) return "请先填写接口地址和视频生成模型。";
    return "";
  }, [providerId, accessKey, secretKey, apiKey, baseUrl, model, isVolcengine]);

  const persistSingleStateProviderConfig = useCallback(() => {
    saveProviderConfig(providerId, {
      apiKey,
      accessKey,
      secretKey,
      baseUrl,
      model,
      thinkingModel,
      fastGeneration,
      advanced,
    });
  }, [providerId, apiKey, accessKey, secretKey, baseUrl, model, thinkingModel, fastGeneration, advanced]);

  const handleSingleStateTestConnection = useCallback(() => {
    const key = providerId === "kling" ? accessKey.trim() : apiKey.trim();
    const normalizedBaseUrl = baseUrl.trim();
    if (isVolcengine) {
      if (!key) {
        setTestFeedback({ tone: "warning", text: "请先填写 API Key。" });
        return;
      }
      setTestFeedback({
        tone: "success",
        text: `火山 Ark 地址已固定，请求会发送到 ${VOLCENGINE_BASE_URL}`,
      });
      return;
    }
    if (!key || !normalizedBaseUrl) {
      setTestFeedback({ tone: "warning", text: "请先填写 API Key 和 Base URL。" });
      return;
    }
    try {
      const url = new URL(
        /^https?:\/\//i.test(normalizedBaseUrl) ? normalizedBaseUrl : `https://${normalizedBaseUrl}`,
      );
      setTestFeedback({
        tone: "success",
        text: `基础地址校验通过，请求会发送到 ${url.origin}`,
      });
    } catch {
      setTestFeedback({ tone: "danger", text: "Base URL 格式不正确。" });
    }
  }, [providerId, apiKey, accessKey, baseUrl, isVolcengine]);

  const handleSingleStateFile = useCallback((file) => {
    if (!file) return;
    if (!isImageFile(file)) {
      setSingleStateStatus("error");
      setSingleStateMessage("请上传 PNG / JPEG / WebP / GIF 图片。");
      setSingleStateProgress(null);
      return;
    }
    setSingleStateImageFile(file);
    setSingleStateStatus("idle");
    setSingleStateMessage("");
    setSingleStateProgress(null);
  }, []);

  const handleSingleStatePickClick = useCallback(() => {
    singleStateInputRef.current?.click();
  }, []);

  const handleSingleStateDrop = useCallback(
    (event) => {
      event.preventDefault();
      const picked = event.dataTransfer.files?.[0];
      if (picked) handleSingleStateFile(picked);
    },
    [handleSingleStateFile],
  );

  const handleOpenSingleStateDialog = useCallback(() => {
    setSingleStateDialogOpen(true);
    setSingleStateStep(0);
  }, []);

  const handleCloseSingleStateDialog = useCallback(() => {
    setSingleStateDialogOpen(false);
  }, []);

  const handleSingleStateGenerate = useCallback(async () => {
    if (!record || !activeRecord?.ok || singleStateStatus === "generating") return;
    if (record.type === "builtin") {
      setSingleStateStatus("error");
      setSingleStateMessage("内置形象不能直接替换单个状态，请先创建自定义形象后再编辑。");
      return;
    }
    if (!singleStateImageFile) {
      setSingleStateStatus("error");
      setSingleStateMessage("请先上传当前状态的参考图片。");
      return;
    }
    const prompt = singleStatePrompt.trim();
    if (!prompt) {
      setSingleStateStatus("error");
      setSingleStateMessage("请填写用于生成当前状态视频的 prompt。");
      return;
    }
    if (singleStateGenerationReadyIssue) {
      setSingleStateStatus("error");
      setSingleStateMessage(singleStateGenerationReadyIssue);
      return;
    }

    persistSingleStateProviderConfig();
    const trimmedModel = model.trim();
    const providerConfig = {
      provider: providerId,
      apiKey,
      accessKey,
      secretKey,
      baseUrl: isVolcengine ? VOLCENGINE_BASE_URL : baseUrl,
      model: trimmedModel,
      thinkingModel: isVolcengine ? VOLCENGINE_THINKING_MODEL : thinkingModel.trim() || trimmedModel,
      fastGeneration,
      advanced:
        providerId === "custom"
          ? {
              ...advanced,
              authHeader: openaiCompat ? "Authorization" : advanced.authHeader,
              authPrefix: openaiCompat ? "Bearer" : advanced.authPrefix,
            }
          : undefined,
    };

    setSingleStateStatus("generating");
    setSingleStateMessage(`正在生成 ${activeRecord.family} 状态素材…`);
    setSingleStateProgress({
      label: "准备生成",
      percent: 6,
      message: `正在生成 ${activeRecord.family} 状态素材…`,
      tone: "running",
    });
    try {
      const result = await runSingleFamilyVideo({
        imageFile: singleStateImageFile,
        family: activeRecord.family,
        prompt,
        providerConfig,
        skipProcessing: !removeBg,
        onProgress: (progress) => {
          if (progress?.message) setSingleStateMessage(progress.message);
          setSingleStateProgress(singleStateProgressFromPipeline(progress, activeRecord.family));
        },
      });
      setSingleStateProgress({
        label: "写入客户端素材",
        percent: 96,
        message: "正在替换本地状态视频…",
        tone: "running",
      });
      const nextRecord = await replaceFamilyVideo({
        appearanceId: record.id,
        family: activeRecord.family,
        videoBytes: result.family.videoBytes,
        taskId: result.family.taskId,
        videoUrl: result.family.videoUrl,
        prompt: result.family.prompt,
      });
      setRecord(nextRecord);
      setSingleStateStatus("success");
      const successMessage = `已替换 ${activeRecord.family} 状态视频文件，已保存到客户端。`;
      setSingleStateMessage(successMessage);
      setSingleStateProgress({
        label: "客户端素材已替换",
        percent: 100,
        message: successMessage,
        tone: "success",
      });
    } catch (generationFailure) {
      console.error(generationFailure);
      setSingleStateStatus("error");
      setSingleStateMessage(generationFailure?.message || String(generationFailure));
      setSingleStateProgress(null);
    }
  }, [
    activeRecord,
    record,
    singleStateImageFile,
    singleStatePrompt,
    singleStateStatus,
    singleStateGenerationReadyIssue,
    persistSingleStateProviderConfig,
    providerId,
    isVolcengine,
    apiKey,
    accessKey,
    secretKey,
    baseUrl,
    model,
    thinkingModel,
    fastGeneration,
    advanced,
    openaiCompat,
    removeBg,
  ]);

  const handleSyncSingleStateToDevice = useCallback(async () => {
    if (!record || singleStateStatus === "syncing") return;
    if (!hasTauriRuntime()) {
      setSingleStateStatus("error");
      setSingleStateMessage("当前不是桌面客户端运行环境，无法替换到板端。");
      return;
    }

    setSingleStateStatus("syncing");
    setSingleStateMessage("正在通过素材 OTA 通道替换到板端…");
    setStateVideoState("saving");
    setStateVideoMessage("正在通过素材 OTA 通道替换到板端…");
    setSingleStateProgress(null);
    try {
      const result = await invoke("usb_sync_appearance", { appearanceId: record.id });
      if (!result?.ok) {
        throw new Error(result?.error || "设备未确认素材替换");
      }
      setSingleStateStatus("synced");
      const message = `已替换到板端（${result.fileCount || 0} 个素材，${result.byteCount || 0} bytes）。`;
      setSingleStateMessage(message);
      setStateVideoState("success");
      setStateVideoMessage(message);
    } catch (syncFailure) {
      console.error(syncFailure);
      setSingleStateStatus("error");
      const message = syncFailure?.message || String(syncFailure);
      setSingleStateMessage(message);
      setStateVideoState("error");
      setStateVideoMessage(message);
    }
  }, [record, singleStateStatus]);

  if (error) {
    return (
      <div className="page page-appearance-detail">
        <div className="page-toolbar">
          <button className="btn-ghost" onClick={onBack}>
            <ArrowLeft size={16} />
            返回宠物图册
          </button>
        </div>
        <div className="message-banner message-banner--error">
          <AlertCircle size={14} /> 读取形象失败：{error}
        </div>
      </div>
    );
  }

  if (!record) {
    return (
      <div className="page page-appearance-detail">
        <div className="empty-state">
          <Loader size={20} className="spin" />
          <div>
            <strong>正在加载形象…</strong>
          </div>
        </div>
      </div>
    );
  }

  const activePreviewMedia = activeRecord?.ok
    ? mediaFromFamily(activeRecord, record) ||
      mediaFromSourcePreview(record) ||
      mediaFromSourceImage(record)
    : null;
  const isBuiltin = record.type === "builtin";
  const canEditAudio = activeRecord?.ok;
  const audioInputId = `audio-cue-${record.id}-${activeFamily || "none"}`;
  const stateVideoInputId = `state-video-${record.id}-${activeFamily || "none"}`;
  const singleStateBusy = singleStateStatus === "generating" || singleStateStatus === "syncing";
  const stateVideoBusy = stateVideoState === "saving";
  const canUploadStateVideo = Boolean(activeRecord) && !isBuiltin && !singleStateBusy && !stateVideoBusy;
  const canRegenerateState = activeRecord?.ok && !isBuiltin && !singleStateBusy;
  const canSyncSingleState = !isBuiltin && (singleStateStatus === "success" || singleStateStatus === "synced");
  const singleStateStartIssue = !singleStateImageFile
    ? "请先上传当前状态的参考图片。"
    : !singleStatePrompt.trim()
      ? "请填写用于生成当前状态视频的 prompt。"
      : singleStateGenerationReadyIssue;

  return (
    <div className="page page-appearance-detail">
      <div className="page-toolbar">
        <button className="btn-ghost" onClick={onBack}>
          <ArrowLeft size={16} />
          返回宠物图册
        </button>
        <span className="grow" />
        {audioSyncDirty && (
          <button
            className="btn-primary detail-audio-sync-btn"
            type="button"
            onClick={handleSyncAudioCues}
            disabled={audioSyncState === "syncing"}
            title="通过板端音效 OTA 通道下发当前 WAV"
          >
            {audioSyncState === "syncing" ? (
              <Loader size={14} className="spin" />
            ) : (
              <UploadCloud size={14} />
            )}
            {audioSyncState === "syncing" ? "下发中..." : "下发音效"}
          </button>
        )}
        {!isBuiltin && (
          <button
            className="btn-ghost danger"
            onClick={handleDelete}
            disabled={deleteState === "deleting"}
          >
            {deleteState === "deleting" ? (
              <>
                <Loader size={14} className="spin" />
                删除中…
              </>
            ) : deleteState === "confirm" ? (
              <>
                <Trash2 size={14} />
                确认删除？
              </>
            ) : (
              <>
                <Trash2 size={14} />
                删除形象
              </>
            )}
          </button>
        )}
      </div>

      {deleteError && (
        <div className="message-banner message-banner--error">
          <AlertCircle size={14} /> 删除形象失败：{deleteError}
        </div>
      )}

      <div className="detail-workspace">
        <div className="detail-preview-panel">
          <div className="detail-media">
            {activeRecord?.ok && activePreviewMedia ? (
              <AppearancePreview
                media={activePreviewMedia}
                className="detail-media__video"
                emptyClassName="detail-media__video detail-media__video--empty"
                playing
              />
            ) : (
              <div
                className="detail-media__video"
                style={{ display: "grid", placeItems: "center", color: "#888", background: "#f4f5f7" }}
              >
                {generatedFamilies.length === 0 ? (
                  <div style={{ textAlign: "center", padding: 16 }}>
                    <AlertCircle size={28} />
                    <div style={{ marginTop: 8 }}>暂无可用动画素材</div>
                    <div className="muted small" style={{ marginTop: 4, maxWidth: 360 }}>
                      这个形象没有成功生成的 family，请返回宠物图册重新创建。
                    </div>
                  </div>
                ) : (
                  <div>暂无可预览素材</div>
                )}
              </div>
            )}
            <div className="detail-media__label">
              <span>{activeRecord?.family || "—"} · {activeRecord?.ok ? "已生成" : "暂无素材"}</span>
              <span>{record.name}</span>
            </div>
          </div>

          <section className="detail-state-rail-section">
            <div className="detail-section-heading">
              <h4>全部状态</h4>
              <span className="muted small">{generatedFamilies.length}/{stateFamilyRecords.length} 个已有素材</span>
            </div>
            {stateFamilyRecords.length === 0 ? (
              <div className="empty-state">
                <div>
                  <strong>暂无可用素材</strong>
                </div>
                <div className="muted small">暂无可编辑状态。</div>
              </div>
            ) : (
              <div className="detail-state-rail">
                {stateFamilyRecords.map((familyRecord) => {
                  const definition =
                    FAMILIES.find((item) => item.family === familyRecord.family) || {
                      family: familyRecord.family,
                      label: familyRecord.family,
                    };
                  const isActive = activeFamily === familyRecord.family;
                  return (
                    <button
                      key={familyRecord.family}
                      type="button"
                      className={`state-card${isActive ? " active" : ""}`}
                      onClick={() => setActiveFamily(familyRecord.family)}
                    >
                      <span className="state-card__thumb">
                        <AppearancePreview
                          media={familyRecord.ok ? mediaFromFamily(familyRecord) : null}
                          className="state-card__media"
                          emptyClassName="state-card__media state-card__media--empty"
                        />
                      </span>
                      <span className="state-card__family">{familyRecord.family}</span>
                      <span className="state-card__hint">{definition.label}</span>
                      <span className="state-card__status">
                        {familyRecord.ok ? "已生成" : "可上传替换"}
                      </span>
                      {(familyRecord.audioPath || familyRecord.audioSrc) && (
                        <span className="state-card__sound">提示音</span>
                      )}
                    </button>
                  );
                })}
              </div>
            )}
          </section>
        </div>

        <aside className="detail-control-panel" aria-label="当前状态控制">
          <div className="detail-control-panel__header">
            <span className="muted small">当前状态</span>
            <strong>{activeRecord?.family || "—"}</strong>
          </div>
          {activeRecord && (
            <section className="detail-side-section detail-side-section--video">
              <div className="detail-state-video-upload">
                <div className="detail-audio-config__header">
                  <span>
                    <UploadCloud size={15} />
                    状态视频
                  </span>
                  <span className="muted small">{activeRecord.family}</span>
                </div>
                <div className="muted small">
                  上传 MP4 直接替换当前状态视频；保存后点击“替换到板端”才会同步到设备。
                </div>
                {stateVideoMessage && (
                  <div
                    className={`message-banner detail-state-regenerate-entry__message ${
                      stateVideoState === "error"
                        ? "message-banner--error"
                        : stateVideoState === "success"
                          ? "message-banner--success"
                          : "message-banner--muted"
                    }`}
                  >
                    {stateVideoBusy ? <Loader size={14} className="spin" /> : <AlertCircle size={14} />}
                    {stateVideoMessage}
                  </div>
                )}
                <div className="detail-state-video-upload__actions">
                  <input
                    id={stateVideoInputId}
                    className="detail-audio-config__input"
                    type="file"
                    accept="video/mp4,.mp4"
                    onChange={handleStateVideoFileChange}
                    disabled={!canUploadStateVideo}
                  />
                  <label
                    className={`btn-ghost${canUploadStateVideo ? "" : " is-disabled"}`}
                    htmlFor={canUploadStateVideo ? stateVideoInputId : undefined}
                  >
                    {stateVideoBusy ? <Loader size={14} className="spin" /> : <Upload size={14} />}
                    上传 MP4 替换
                  </label>
                  {canSyncSingleState && (
                    <button
                      className="btn-primary"
                      type="button"
                      onClick={handleSyncSingleStateToDevice}
                      disabled={singleStateBusy}
                    >
                      {singleStateStatus === "syncing" ? <Loader size={14} className="spin" /> : <UploadCloud size={14} />}
                      替换到板端
                    </button>
                  )}
                </div>
                {isBuiltin && (
                  <div className="muted small">
                    内置形象不能直接替换状态视频，请先创建自定义形象后再编辑。
                  </div>
                )}
              </div>
            </section>
          )}
          {activeRecord?.ok && (
            <section className="detail-side-section detail-side-section--audio">
              <div className="detail-audio-config">
                <div className="detail-audio-config__header">
                  <span>
                    <Volume2 size={15} />
                    状态提示音
                  </span>
                  <span className="muted small">{activeRecord.family}</span>
                </div>
                {audioUrl ? (
                  <audio className="detail-audio-config__player" src={audioUrl} controls />
                ) : (
                  <div className="muted small">
                    当前状态 {activeRecord.family} 还没有提示音。
                    {audioFamilyNames.length > 0
                      ? ` 已有提示音: ${audioFamilyNames.join(" / ")}。请选择对应状态试听，或为当前状态上传 WAV。`
                      : ""}
                  </div>
                )}
                {audioErr && (
                  <div className="message-banner message-banner--error">
                    <AlertCircle size={14} /> {audioErr}
                  </div>
                )}
                {(audioSyncDirty || audioSyncMessage) && (
                  <div
                    className={`message-banner detail-audio-sync-message ${
                      audioSyncState === "error"
                        ? "message-banner--error"
                        : audioSyncState === "success"
                          ? "message-banner--success"
                          : "message-banner--warning"
                    }`}
                  >
                    {audioSyncState === "syncing" ? <Loader size={14} className="spin" /> : <UploadCloud size={14} />}
                    {audioSyncMessage || "音效已保存到客户端，需要通过板端音效 OTA 通道下发到设备后才会生效。"}
                  </div>
                )}
                <div className="detail-audio-config__actions">
                  <input
                    id={audioInputId}
                    className="detail-audio-config__input"
                    type="file"
                    accept="audio/wav,audio/x-wav,.wav"
                    onChange={handleAudioFileChange}
                    disabled={!canEditAudio || audioState === "saving"}
                  />
                  <label
                    className={`btn-ghost${canEditAudio ? "" : " is-disabled"}`}
                    htmlFor={canEditAudio ? audioInputId : undefined}
                  >
                    {audioState === "saving" ? <Loader size={14} className="spin" /> : <Upload size={14} />}
                    上传 WAV
                  </label>
                  {audioUrl && canEditAudio && !activeRecord.audioDefault && (
                    <button
                      className="btn-ghost danger"
                      type="button"
                      onClick={handleRemoveAudioCue}
                      disabled={audioState === "saving"}
                    >
                      <Trash2 size={14} />
                      移除提示音
                    </button>
                  )}
                </div>
                {isBuiltin && (
                  <div className="muted small">
                    未上传时使用默认提示音；上传后会优先使用你的配置。
                  </div>
                )}
                {!isBuiltin && activeRecord.audioDefault && (
                  <div className="muted small">
                    未上传时使用默认提示音；上传后会优先使用你的配置。
                  </div>
                )}
              </div>
            </section>
          )}
          {activeRecord?.ok && (
            <section className="detail-side-section detail-side-section--regenerate">
              <div className="detail-state-regenerate-entry">
                <div className="detail-state-regenerate-entry__header">
                  <span className="detail-state-regenerate-entry__title">
                    <ImageUp size={15} />
                    单状态重生成
                  </span>
                  <span className="muted small">{activeRecord.family}</span>
                </div>
                <div className="muted small">
                  上传一张参考图并调整 prompt，只重新生成当前状态；生成配置沿用创建形象的向导样式。
                </div>
                <div className="detail-state-regenerate-entry__actions">
                  <button
                    className="btn-ghost"
                    type="button"
                    onClick={handleOpenSingleStateDialog}
                    disabled={!canRegenerateState}
                  >
                    {singleStateBusy ? <Loader size={14} className="spin" /> : <ImageUp size={14} />}
                    <span>重新生成当前状态</span>
                  </button>
                  {canSyncSingleState && (
                    <button
                      className="btn-primary"
                      type="button"
                      onClick={handleSyncSingleStateToDevice}
                      disabled={singleStateBusy}
                    >
                      {singleStateStatus === "syncing" ? <Loader size={14} className="spin" /> : <UploadCloud size={14} />}
                      替换到板端
                    </button>
                  )}
                </div>
                {(singleStateMessage || isBuiltin) && (
                  <div
                    className={`message-banner detail-state-regenerate-entry__message ${
                      singleStateStatus === "error"
                        ? "message-banner--error"
                        : singleStateStatus === "success" || singleStateStatus === "synced"
                          ? "message-banner--success"
                          : "message-banner--muted"
                    }`}
                  >
                    {singleStateStatus === "generating" || singleStateStatus === "syncing" ? (
                      <Loader size={14} className="spin" />
                    ) : (
                      <AlertCircle size={14} />
                    )}
                    {singleStateMessage || "内置形象不能直接替换单个状态，请先创建自定义形象后再编辑。"}
                  </div>
                )}
              </div>
            </section>
          )}
        </aside>
      </div>

      <details className="detail-context-drawer" open>
        <summary>
          <span>形象信息</span>
          <span className="muted small">来源、模型与生成路径</span>
        </summary>
        <section className="detail-summary-card">
          <h2>{record.name}</h2>
          {record.description && <div className="detail-summary-card__description">{record.description}</div>}
          <div className="detail-summary-card__meta">
            <span className="pill" style={{ background: "var(--accent-soft)", color: "var(--accent-strong)" }}>
              {appearanceSourceLabel(record)}
            </span>
            <span className="muted small">
              {record.provider || "—"} · {record.model || "—"}
            </span>
            <span className="muted small">
              生成时间：{new Date(record.created_at).toLocaleString()}
            </span>
          </div>
          <div className="path">{record.absolute_dir}</div>
        </section>
      </details>

      {singleStateDialogOpen && activeRecord?.ok && (
        <div className="modal-backdrop" onClick={handleCloseSingleStateDialog}>
          <div
            className="modal-card modal-card--wide single-state-regenerate-modal"
            role="dialog"
            aria-modal="true"
            aria-labelledby="single-state-regenerate-title"
            onClick={(event) => event.stopPropagation()}
          >
            <div className="modal-header">
              <div>
                <h3 id="single-state-regenerate-title" className="modal-title">
                  单状态重生成
                </h3>
                <div className="modal-subtitle">
                  当前状态：{activeRecord.family}。关闭弹窗不会取消生成，完成后只替换客户端视频文件；需要设备生效时，关闭弹窗后手动点击“替换到板端”。
                </div>
              </div>
              <button
                className="icon-btn"
                type="button"
                onClick={handleCloseSingleStateDialog}
                aria-label="关闭单状态重生成"
              >
                ×
              </button>
            </div>
            <div className="modal-body">
              <div className="ca-wizard single-state-regenerate-modal__wizard">
                <div className="ca-tabs">
                  {["上传参考图", "生成配置"].map((label, index) => (
                    <div
                      key={label}
                      className={`ca-tab ${singleStateStep === index ? "active" : singleStateStep > index ? "done" : ""}`}
                    >
                      {singleStateStep > index ? "OK" : index + 1} · {label}
                    </div>
                  ))}
                </div>

                {singleStateStep === 0 && (
                  <AvatarWizardStep1
                    file={singleStateImageFile}
                    previewUrl={singleStateImagePreview}
                    appearanceName={record.name}
                    personality=""
                    onAppearanceName={() => {}}
                    onPersonality={() => {}}
                    onPickClick={handleSingleStatePickClick}
                    onDrop={handleSingleStateDrop}
                    inputRef={singleStateInputRef}
                    onFileChange={handleSingleStateFile}
                    onCancel={handleCloseSingleStateDialog}
                    canAdvance={Boolean(singleStateImageFile) && !isBuiltin}
                    onNext={() => setSingleStateStep(1)}
                    title={`第 1 步 · 上传 ${activeRecord.family} 参考图`}
                    identityFields={false}
                    cancelLabel="关闭"
                    nextLabel="下一步：生成配置"
                  >
                    <div className="field single-state-regenerate-modal__prompt">
                      <label className="field-label">当前状态 prompt</label>
                      <textarea
                        className="field-input"
                        value={singleStatePrompt}
                        onChange={(event) => setSingleStatePrompt(event.target.value)}
                        disabled={singleStateBusy}
                        placeholder="描述这个状态要生成的动作，例如：小八猫趴在纸箱里认真写字，保持黑色背景，循环动画。"
                      />
                    </div>
                  </AvatarWizardStep1>
                )}

                {singleStateStep === 1 && (
                  <AvatarWizardStep2
                    providerId={providerId}
                    apiKey={apiKey}
                    onApiKey={setApiKey}
                    accessKey={accessKey}
                    onAccessKey={setAccessKey}
                    secretKey={secretKey}
                    onSecretKey={setSecretKey}
                    baseUrl={baseUrl}
                    onBaseUrl={setBaseUrl}
                    model={model}
                    onModel={setModel}
                    advancedOpen={advancedOpen}
                    onAdvancedOpen={setAdvancedOpen}
                    openaiCompat={openaiCompat}
                    onOpenaiCompat={setOpenaiCompat}
                    advanced={advanced}
                    onAdvanced={setAdvanced}
                    testFeedback={testFeedback}
                    onPickProvider={setProviderId}
                    onTestConnection={handleSingleStateTestConnection}
                    canStart={!singleStateStartIssue && canRegenerateState}
                    generationReadyIssue={singleStateStartIssue}
                    submitError={singleStateStatus === "error" ? singleStateMessage : ""}
                    removeBg={removeBg}
                    onRemoveBg={setRemoveBg}
                    fastGeneration={fastGeneration}
                    onFastGeneration={setFastGeneration}
                    onBack={() => setSingleStateStep(0)}
                    onStart={handleSingleStateGenerate}
                    progress={
                      singleStateStatus !== "error" && singleStateProgress
                        ? singleStateProgress
                        : null
                    }
                    title={`第 2 步 · 生成 ${activeRecord.family}`}
                    startLabel="生成并替换客户端视频"
                  />
                )}

                {singleStateStatus !== "error" && singleStateMessage && !singleStateProgress && (
                  <div
                    className={`message-banner single-state-regenerate-modal__message ${
                      singleStateStatus === "success" || singleStateStatus === "synced"
                        ? "message-banner--success"
                        : "message-banner--muted"
                    }`}
                  >
                    {singleStateBusy ? <Loader size={14} className="spin" /> : <AlertCircle size={14} />}
                    {singleStateMessage}
                  </div>
                )}
              </div>
            </div>
            <div className="single-state-regenerate-modal__footer">
              <button
                className="btn-ghost"
                type="button"
                onClick={handleCloseSingleStateDialog}
              >
                关闭
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
